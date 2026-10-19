"""Generate short synthetic Dolby fixtures; no private media is read."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
output = root / 'test-artifacts/audio-probe-fixtures'
output.mkdir(parents=True, exist_ok=True)
ffmpeg = os.environ.get('STREAMDESK_FFMPEG') or shutil.which('ffmpeg')
assert ffmpeg, 'Set STREAMDESK_FFMPEG'
for name, codec, layout, bitrate, pid in (
    ('synthetic-ac3.ts', 'ac3', '5.1', '640k', 4352),
    ('synthetic-eac3.ts', 'eac3', 'stereo', '384k', 4353),
):
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i',
                    f'anullsrc=r=48000:cl={layout}', '-t', '4', '-c:a', codec,
                    '-b:a', bitrate, '-streamid', f'0:{pid}', '-f', 'mpegts', str(output/name)],
                   check=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
print('Synthetic AC-3/E-AC-3 fixtures ready in test-artifacts/audio-probe-fixtures.')
