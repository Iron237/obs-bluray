import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest
import numpy as np
from scipy.signal import correlate
ROOT=Path(__file__).resolve().parents[1]
ART=ROOT/'test-artifacts/obs'
PLUGIN=ROOT/'dist/OBS-BluRay-0.1.1/obs-bluray'
OBS=Path(os.environ.get('OBS_ROOT',str(Path(os.environ['ProgramFiles'])/'obs-studio')))
FFMPEG=Path(os.environ.get('STREAMDESK_FFMPEG',shutil.which('ffmpeg') or 'ffmpeg'))
HIDE=subprocess.CREATE_NO_WINDOW
RESULTS=[]
def run(args,**kwargs):return subprocess.run([str(x) for x in args],creationflags=HIDE,**kwargs)
def quality(pcm, reference, start):
    data = np.fromfile(pcm, dtype='<f4').reshape(-1, 2)[:, 0].astype(float)
    nonzero = np.flatnonzero(np.abs(data) > .0002)
    assert len(nonzero), f'OBS mixed output is silent: {pcm}'
    # OBS establishes its initial A/V synchronization during activation. Assess
    # conversion quality after a fixed 500 ms warm-up, never by picking the
    # best-scoring window. Still reject non-finite samples or clipping anywhere.
    assert np.isfinite(data).all(), f'Non-finite PCM: {pcm}'
    begin = int(nonzero[0]) + 24000
    actual = data[begin:begin + 48000]
    assert len(actual) == 48000, len(actual)
    r = run([FFMPEG, '-v', 'error', '-ss', str(start), '-i', reference, '-t', '30', '-vn', '-ar', '48000', '-ac', '2', '-f', 'f32le', '-'], capture_output=True, check=True, timeout=45)
    ref = np.frombuffer(r.stdout, dtype='<f4').reshape(-1, 2)[:, 0].astype(float)
    offset = int(np.argmax(correlate(ref, actual, mode='valid', method='fft')))
    match = ref[offset:offset + len(actual)]
    correlation = float(np.corrcoef(match, actual)[0, 1])
    gain = float(np.dot(match, actual) / max(np.dot(match, match), 1e-12))
    snr = float(10 * np.log10(max(np.mean((match * gain) ** 2), 1e-15) / max(np.mean((actual-match*gain)**2), 1e-15)))
    return dict(correlation=correlation, snrDb=snr, gain=gain, clippedFraction=float(np.mean(np.abs(data) >= 1)), onsetSeconds=float(nonzero[0]/48000), warmupSeconds=.5, measuredSeconds=1)

class ObsPluginTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not FFMPEG.is_file():raise unittest.SkipTest('Set STREAMDESK_FFMPEG')
        ART.mkdir(parents=True,exist_ok=True)
        run([FFMPEG,'-v','error','-y','-f','lavfi','-i','color=red:s=640x360:r=30','-f','lavfi','-i','aevalsrc=0.1*sin(2*PI*(440*t+12*t*t)):s=48000','-t','45','-c:v','libx264','-preset','ultrafast','-c:a','pcm_s24le',ART/'red.mkv'],check=True,timeout=30)
        if os.environ.get('RUN_HARDWARE_TESTS')=='1':
            run([FFMPEG,'-v','error','-y','-i',ART/'red.mkv','-c:v','hevc_nvenc','-c:a','copy',ART/'red-hevc.mkv'],check=True,timeout=30)
        (ART/'test.ass').write_text((ROOT/'tests/fixtures/obs.ass').read_text(encoding='utf-8'),encoding='utf-8')
    def host(self, name, source, subtitle='', decoder='software', start=0, **settings):
        env = os.environ.copy()
        env['PATH'] = f'{OBS}/bin/64bit;C:/Windows/System32;C:/Windows'
        env['STREAMDESK_DATA'] = str(ART / ('data-' + name))
        with open(ART/(name+'.log'), 'w', encoding='utf-8') as log:
            result = run([ROOT/'build/obs-tests/obs-source-tests.exe', PLUGIN/'bin/64bit/obs-bluray.dll', PLUGIN/'data', source, ART/name, subtitle, decoder, str(start), json.dumps(settings)], env=env, stdout=log, stderr=log, timeout=75)
        text = (ART/(name+'.log')).read_text(encoding='utf-8', errors='replace')
        self.assertEqual(result.returncode, 0, text[-3000:])
        self.assertNotIn('audio is lagging', text)
        self.assertNotIn('Max audio buffering reached', text)
        if settings.get('_expectError'):
            RESULTS.append(dict(name=name, invalidSourceRejected=True))
            return
        counts = json.loads((ART/(name+'.json')).read_text())
        self.assertGreater(counts['videoFrames'], 20)
        self.assertGreater(counts['audioFrames'], 48000)
        if subtitle:
            self.assertGreater(counts['subtitleFrames'], 0)
            self.assertGreater(counts['transparentFrames'], 0)
        ref = source
        q = quality(ART/(name+'.mix.f32'), ref, start)
        self.assertGreater(q['correlation'], .98, q)
        self.assertGreater(q['snrDb'], 15, q)
        self.assertEqual(q['clippedFraction'], 0)
        RESULTS.append(dict(name=name, decoder=decoder, controls=True, **counts, audioQuality=q))
    def test_01_cpu_source_and_separate_ass(self):
        self.host('cpu', ART/'red.mkv', ART/'test.ass')
    @unittest.skipUnless(os.environ.get('RUN_HARDWARE_TESTS')=='1','Set RUN_HARDWARE_TESTS=1 on a supported Intel/NVIDIA host')
    def test_02_intel_source_and_separate_ass(self):
        self.host('intel', ART/'red.mkv', ART/'test.ass', 'intel')
    def test_05_invalid_source(self):
        self.host('missing', ART/'does-not-exist', _expectError=True)
    @unittest.skipUnless(os.environ.get('RUN_HARDWARE_TESTS')=='1','Set RUN_HARDWARE_TESTS=1 on a supported Intel/NVIDIA host')
    def test_08_hevc_input_nvidia(self):
        self.host('hevc-input', ART/'red-hevc.mkv', ART/'test.ass', 'nvidia')

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ObsPluginTests))
    (ART/'results.json').write_text(json.dumps(dict(ok=result.wasSuccessful(),tests=result.testsRun,results=RESULTS),ensure_ascii=False,indent=2),encoding='utf-8')
    raise SystemExit(0 if result.wasSuccessful() else 1)
