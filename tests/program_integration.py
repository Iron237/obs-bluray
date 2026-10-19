"""Native worker integration: synthetic BDMV, real IPC, decoder and libass.

This bypasses OBS only to inspect each generation before its presentation queue.
No metadata overrides, network services, actual discs or live OBS are used.
"""
import contextlib
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import subprocess
import time
import unittest
import uuid
import numpy as np
from scipy.signal import correlate
from bluray_fixture import create_disc

ROOT=Path(__file__).resolve().parents[1]
ART=ROOT/'test-artifacts/program'
WORKER=ROOT/'build/bin/obs-bluray-worker.exe'
FFMPEG=Path(os.environ.get('STREAMDESK_FFMPEG','ffmpeg'))
MAX_VIDEO=4096*2160*4

class Audio(C.Structure):
    _fields_=[('timestamp',C.c_uint64),('bytes',C.c_uint32),('data',C.c_ubyte*65536)]
class Shared(C.Structure):
    _fields_=[('signature',C.c_uint32),('version',C.c_uint32),('videoSequence',C.c_uint64),('videoTimestamp',C.c_uint64),
        ('width',C.c_uint32),('height',C.c_uint32),('stride',C.c_uint32),('videoBytes',C.c_uint32),
        ('audioWrite',C.c_uint64),('audioRead',C.c_uint64),('audioDropped',C.c_uint64),('commandSequence',C.c_uint64),
        ('command',C.c_uint32),('state',C.c_uint32),('commandValue',C.c_int64),('positionMs',C.c_int64),('durationMs',C.c_int64),
        ('subtitleFrames',C.c_uint64),('separateSubtitles',C.c_uint32),('error',C.c_char*1024),
        ('video',C.c_ubyte*MAX_VIDEO),('subtitle',C.c_ubyte*MAX_VIDEO),('audio',Audio*64),
        ('generation',C.c_uint64),('episodeIndex',C.c_int32),('episodeCount',C.c_uint32),('episodeId',C.c_char*128),('episodeLabel',C.c_char*512),('playlist',C.c_int32)]
k=C.WinDLL('kernel32',use_last_error=True)
k.CreateFileMappingW.argtypes=[W.HANDLE,C.c_void_p,W.DWORD,W.DWORD,W.DWORD,W.LPCWSTR];k.CreateFileMappingW.restype=W.HANDLE
k.MapViewOfFile.argtypes=[W.HANDLE,W.DWORD,W.DWORD,W.DWORD,C.c_size_t];k.MapViewOfFile.restype=C.c_void_p
k.CreateMutexW.argtypes=[C.c_void_p,W.BOOL,W.LPCWSTR];k.CreateMutexW.restype=W.HANDLE
k.WaitForSingleObject.argtypes=[W.HANDLE,W.DWORD];k.WaitForSingleObject.restype=W.DWORD
k.ReleaseMutex.argtypes=[W.HANDLE];k.CloseHandle.argtypes=[W.HANDLE];k.UnmapViewOfFile.argtypes=[C.c_void_p]

class Worker:
    def __init__(self,program,subtitle):
        self.name='Local\\OBS-BluRay-Program-Test-'+str(uuid.uuid4())
        self.mapping=k.CreateFileMappingW(W.HANDLE(-1),None,4,0,C.sizeof(Shared),self.name)
        self.address=k.MapViewOfFile(self.mapping,0xf001f,0,0,C.sizeof(Shared));assert self.address,C.get_last_error()
        self.h=Shared.from_address(self.address);self.h.signature=0x53444f31;self.h.version=2
        self.mutex=k.CreateMutexW(None,False,self.name+'-lock');assert self.mutex
        self.samples={};self.frames=[];self.last_sequence=0
        settings={'input':str(ART/'disc'),'audioTrack':0,'decoder':'software','separateSubtitles':True,'programJson':json.dumps(program)}
        config=ART/(str(uuid.uuid4())+'.json');config.write_text(json.dumps(settings),encoding='utf-8')
        env=os.environ.copy();env['STREAMDESK_DATA']=str(ART/'data')
        self.log=open(ART/(str(uuid.uuid4())+'.log'),'w',encoding='utf-8')
        self.process=subprocess.Popen([str(WORKER),'--map',self.name,'--config',str(config),'--parent',str(os.getpid())],env=env,stdout=self.log,stderr=self.log,creationflags=subprocess.CREATE_NO_WINDOW)
    @contextlib.contextmanager
    def lock(self):
        result=k.WaitForSingleObject(self.mutex,2000);assert result in (0,128),result
        try:yield self.h
        finally:k.ReleaseMutex(self.mutex)
    def poll(self):
        with self.lock() as h:
            if h.state==5:raise AssertionError(bytes(h.error).decode('utf-8','replace'))
            s=dict(state=h.state,position=h.positionMs,duration=h.durationMs,generation=h.generation,episode=bytes(h.episodeId).decode(),playlist=h.playlist,index=h.episodeIndex,count=h.episodeCount,dropped=h.audioDropped,separate=bool(h.separateSubtitles))
            if h.videoSequence!=self.last_sequence and h.videoBytes:
                self.last_sequence=h.videoSequence
                pixel=bytes(h.video[:4]);alpha=bool(np.any(np.ctypeslib.as_array(h.subtitle)[:h.videoBytes].reshape(-1,4)[:,3])) if h.separateSubtitles else False
                self.frames.append(dict(**s,pixel=list(pixel),alpha=alpha,timestamp=h.videoTimestamp))
            while h.audioRead<h.audioWrite:
                a=h.audio[h.audioRead%64];self.samples.setdefault(h.generation,[]).append(bytes(a.data[:a.bytes]));h.audioRead+=1
            return s
    def wait(self,predicate,timeout=18):
        until=time.monotonic()+timeout;last={}
        while time.monotonic()<until:
            last=self.poll()
            if predicate(last):return last
            if self.process.poll() is not None:raise AssertionError(f'worker exited {self.process.returncode}; last={last}')
            time.sleep(.003)
        raise AssertionError(f'timeout: {last}')
    def run_for(self,seconds):
        until=time.monotonic()+seconds
        while time.monotonic()<until:self.poll();time.sleep(.003)
    def command(self,command,value=0):
        with self.lock() as h:h.command=command;h.commandValue=value;h.commandSequence+=1
    def close(self):
        self.process.terminate();self.process.wait(5);self.log.close();del self.h;k.UnmapViewOfFile(self.address);k.CloseHandle(self.mapping);k.CloseHandle(self.mutex)
    def __enter__(self):return self
    def __exit__(self,*args):self.close()

RESULTS=[]
class ProgramIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ART.mkdir(parents=True,exist_ok=True);create_disc(ART/'disc',FFMPEG)
        cls.subtitle=ART/'absolute.ass'
        cls.subtitle.write_text('[Script Info]\nScriptType: v4.00+\nPlayResX: 320\nPlayResY: 180\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,24,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:01.20,0:00:01.80,Default,,0,0,0,,FIRST\nDialogue: 0,0:00:05.20,0:00:05.80,Default,,0,0,0,,SECOND\n',encoding='utf-8')
        p=subprocess.run([str(FFMPEG),'-v','error','-i',str(ART/'disc/BDMV/STREAM/00000.m2ts'),'-vn','-ar','48000','-ac','2','-f','f32le','-'],capture_output=True,check=True,creationflags=subprocess.CREATE_NO_WINDOW)
        cls.reference=np.frombuffer(p.stdout,dtype='<f4').reshape(-1,2)[:,0].astype(float)
    def program(self,mode='sequence',loop=False):
        return dict(version=1,selectedEpisode='a',playMode=mode,loop=loop,episodes=[
            dict(id='a',label='First range',enabled=True,playlist=1,start=1,end=3),
            dict(id='disabled',label='Skipped',enabled=False,playlist=1,start=3,end=4),
            dict(id='b',label='Second range',enabled=True,playlist=1,start=5,end=7),
            dict(id='c',label='Other playlist',enabled=True,playlist=2,start=9,end=11)],
            subtitles=[dict(playlist=1,mode='external',track=-1,file=str(self.subtitle),offset=0,fontsDir=''),dict(playlist=2,mode='off',track=-1,file='',offset=0,fontsDir='')])
    def test_01_sequence_ranges_and_absolute_ass(self):
        with Worker(self.program(),self.subtitle) as w:
            end=w.wait(lambda s:s['state']==4,timeout=45)
            self.assertEqual(end['episode'],'c');self.assertEqual(end['duration'],2000);self.assertEqual(end['position'],2000);self.assertEqual(end['count'],3);self.assertEqual(end['dropped'],0)
            groups={e:[f for f in w.frames if f['episode']==e] for e in ('a','b','c')}
            checks=[]
            for episode,start,color in [('a',1,2),('b',5,1),('c',9,0)]:
                frames=groups[episode];self.assertGreater(len(frames),30);self.assertTrue(all(f['pixel'][color]>80 for f in frames),frames[:2])
                self.assertLess(min(f['position'] for f in frames),100);self.assertLessEqual(max(f['position'] for f in frames),2000)
                if episode!='c':
                    self.assertTrue(any(f['alpha'] and 100<=f['position']<=900 for f in frames));self.assertFalse(any(f['alpha'] and f['position']>1000 for f in frames))
                else:self.assertFalse(any(f['separate'] or f['alpha'] for f in frames))
                raw=b''.join(w.samples[frames[0]['generation']]);data=np.frombuffer(raw,dtype='<f4').reshape(-1,2)[:,0].astype(float)
                self.assertGreater(len(data),48000*1.94);self.assertLessEqual(len(data),48000*2+2)
                # Content match tests the real MPLS clock, independently of the
                # position field. Wrong early seek preroll must not leak out.
                actual=data[:12000];offset=int(np.argmax(correlate(self.reference,actual,mode='valid',method='fft')));corr=float(np.corrcoef(self.reference[offset:offset+len(actual)],actual)[0,1])
                self.assertGreater(corr,.98);self.assertLess(abs(offset/48000-start),.035)
                checks.append(dict(episode=episode,frames=len(frames),samples=len(data),firstAudioContentSeconds=offset/48000,correlation=corr,subtitleFrames=sum(f['alpha'] for f in frames)))
            RESULTS.append(dict(test='sequence',checks=checks,endedRelativeMs=end['position']))
    def test_02_controls(self):
        with Worker(self.program(mode='single'),self.subtitle) as w:
            s=w.wait(lambda s:s['episode']=='a' and s['state']==1 and s['position']>200)
            w.command(1);paused=w.wait(lambda s:s['state']==2);w.run_for(.3);self.assertLess(abs(w.poll()['position']-paused['position']),70)
            old=paused['generation'];w.command(3,1100);s=w.wait(lambda s:s['generation']>old and s['state']==2 and s['position']>=1050);self.assertLess(abs(s['position']-1100),60)
            previews=[f for f in w.frames if f['generation']==s['generation']]
            self.assertTrue(previews,'Paused seek must decode its new preview, not just change a position field')
            self.assertFalse(any(f['alpha'] for f in previews),'ASS at 1.2s must not leak into the 2.1s seek target')
            self.assertFalse(w.samples.get(s['generation']),'A paused seek must not emit audio while scanning to its target')
            w.command(6);s=w.wait(lambda s:s['episode']=='b' and s['state']==2);self.assertLess(s['position'],70)
            self.assertTrue(any(f['generation']==s['generation'] for f in w.frames))
            self.assertFalse(w.samples.get(s['generation']))
            w.command(7);s=w.wait(lambda s:s['episode']=='a' and s['state']==2);self.assertLess(s['position'],70)
            old=s['generation'];w.command(5);s=w.wait(lambda s:s['generation']>old and s['state']==1 and s['position']>100)
            w.command(4);s=w.wait(lambda s:s['state']==3);self.assertEqual(s['episode'],'a')
            old=s['generation'];w.command(2);s=w.wait(lambda s:s['generation']>old and s['state']==1 and s['position']>100)
            end=w.wait(lambda s:s['state']==4);self.assertEqual(end['episode'],'a');self.assertEqual(end['position'],2000)
            old=end['generation'];w.command(5);s=w.wait(lambda s:s['generation']>old and s['state']==1 and s['position']>100)
            RESULTS.append(dict(test='controls',pauseSeekRelativeMs=1100,manualNextPrevious=True,stopResume=True,singleEnded=True,restartEnded=True))
    def test_03_single_loop(self):
        with Worker(self.program(mode='single',loop=True),self.subtitle) as w:
            s=w.wait(lambda s:s['state']==1 and s['position']>200);old=s['generation']
            s=w.wait(lambda s:s['generation']>old and s['state']==1 and s['position']>100)
            self.assertEqual(s['episode'],'a');RESULTS.append(dict(test='singleLoop',sameEpisode=True))
    def test_04_sequence_loop_wrap(self):
        program=self.program(loop=True)
        program['episodes']=program['episodes'][:3]
        program['episodes'][0].update(start=0,end=.5)
        program['episodes'][2].update(start=1,end=1.5)
        with Worker(program,self.subtitle) as w:
            first=w.wait(lambda s:s['episode']=='a' and s['state']==1 and s['position']>100)
            w.wait(lambda s:s['episode']=='b' and s['state']==1)
            wrapped=w.wait(lambda s:s['episode']=='a' and s['generation']>first['generation'] and s['state']==1 and s['position']>100)
            self.assertEqual(wrapped['count'],2)
            RESULTS.append(dict(test='sequenceLoop',disabledSkipped=True,wrapToFirst=True))

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProgramIntegration))
    (ART/'results.json').write_text(json.dumps(dict(ok=result.wasSuccessful(),tests=result.testsRun,results=RESULTS),indent=2),encoding='utf-8')
    raise SystemExit(0 if result.wasSuccessful() else 1)
