"""Generate a tiny unencrypted test disc from synthetic video/audio only.

The minimal navigation records follow libbluray's MPLS/CLPI readers. The CPI
has one entry at the beginning, exercising precise worker range filtering
after an imprecise underlying seek.
"""
from pathlib import Path
import os
import struct
import subprocess


def u16(n): return struct.pack('>H', n)
def u32(n): return struct.pack('>I', n)
def block(payload): return u32(len(payload)) + payload


def playlist(seconds):
    def stream(pid, attributes):
        return bytes([3, 1]) + u16(pid) + bytes([len(attributes)]) + attributes
    stn = b'\0\0' + bytes([1, 1, 0, 0, 0, 0, 0, 0]) + b'\0'*4
    stn += stream(0x1011, bytes([0x1b, 0x53]))
    stn += stream(0x1100, bytes([0x80, 0x31]) + b'eng')
    item = b'00000M2TS' + u16(1) + b'\0' + u32(0) + u32(int(seconds*45000))
    item += b'\0'*8 + b'\x80\0\0\0' + u16(len(stn)) + stn
    listing = block(b'\0\0' + u16(1) + u16(0) + u16(len(item)) + item)
    app = block(b'\0\x01\0\0' + b'\0'*8 + b'\x80\0')
    start = 40 + len(app)
    marks = block(u16(1) + b'\0\x01' + u16(0) + u32(0) + u16(0x1011) + u32(0))
    return b'MPLS0200' + u32(start) + u32(start+len(listing)) + u32(0) + b'\0'*20 + app + listing + marks


def clip_info(packet_count, seconds):
    clip = block(b'\0\0\x01\x01' + u32(0) + u32(200000) + u32(packet_count) + b'\0'*128 + u16(0))
    seq = block(b'\0\x01' + u32(0) + b'\x01\0' + u16(0x1001) + u32(0) + u32(0) + u32(int(seconds*45000)))
    # Stream attributes include the mandatory 12-byte ISRC field.
    video = bytes([0x1b, 0x53, 0x30, 0, 0]) + b'\0'*12
    audio = bytes([0x80, 0x31]) + b'eng' + b'\0'*12
    program = block(b'\0\x01' + u32(0) + u16(0x100) + b'\x02\0' + u16(0x1011) + bytes([len(video)]) + video + u16(0x1100) + bytes([len(audio)]) + audio)
    def bits(*fields):
        value=0; size=0
        for number,width in fields:value=(value<<width)|number;size+=width
        assert size%8==0
        return value.to_bytes(size//8,'big')
    entry=bits((0x1011,16),(0,10),(1,4),(1,16),(1,18),(14,32))
    cpi=block(u16(1)+b'\0\x01'+entry+u32(12)+u32(0)+u32(0)+u32(0x80000000))
    sequence_at=40+len(clip); program_at=sequence_at+len(seq); cpi_at=program_at+len(program)
    return b'HDMV0200' + u32(sequence_at) + u32(program_at) + u32(cpi_at) + u32(cpi_at+len(cpi)) + u32(0) + b'\0'*12 + clip + seq + program + cpi + u32(0)


def create_disc(path, ffmpeg):
    root=Path(path); bd=root/'BDMV'
    for directory in ['PLAYLIST','CLIPINF','STREAM']:(bd/directory).mkdir(parents=True,exist_ok=True)
    clip=bd/'STREAM/00000.m2ts'
    subprocess.run([str(ffmpeg),'-v','error','-y','-f','lavfi','-i',
        'color=red:s=320x180:r=24,drawbox=x=0:y=0:w=iw:h=ih:color=green:t=fill:enable=gte(t\\,4),drawbox=x=0:y=0:w=iw:h=ih:color=blue:t=fill:enable=gte(t\\,8)',
        '-f','lavfi','-i','aevalsrc=0.12*sin(2*PI*(220*t+20*t*t)):s=48000',
        '-t','12','-c:v','libx264','-preset','ultrafast','-g','24','-bf','0','-pix_fmt','yuv420p',
        '-c:a','pcm_bluray','-ar','48000','-ac','2','-streamid','0:0x1011','-streamid','1:0x1100',
        '-mpegts_m2ts_mode','1','-muxdelay','0','-muxpreload','0','-f','mpegts',str(clip)],check=True,creationflags=subprocess.CREATE_NO_WINDOW)
    (bd/'CLIPINF/00000.clpi').write_bytes(clip_info(clip.stat().st_size//192,12))
    for number in (1,2):(bd/f'PLAYLIST/{number:05}.mpls').write_bytes(playlist(12))
    return root

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('output');args=parser.parse_args()
    create_disc(args.output,os.environ.get('STREAMDESK_FFMPEG','ffmpeg'))
