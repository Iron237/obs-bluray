"""Exercise real OBS program-property callbacks using a synthetic disc."""
import json
import os
from pathlib import Path
import shutil
import subprocess
from bluray_fixture import create_disc

root = Path(__file__).resolve().parents[1]
art = root / 'test-artifacts/program-properties'
art.mkdir(parents=True, exist_ok=True)
obs = os.environ.get('OBS_ROOT')
ffmpeg = os.environ.get('STREAMDESK_FFMPEG') or shutil.which('ffmpeg')
assert obs and ffmpeg, 'Set OBS_ROOT and STREAMDESK_FFMPEG'
disc = create_disc(art/'disc', ffmpeg)
program = dict(version=1, selectedEpisode='a', playMode='sequence', loop=False,
    episodes=[dict(id=name, label=name, enabled=True, playlist=playlist, start=start, end=start+2)
              for name, playlist, start in [('a', 1, 1), ('b', 1, 5), ('c', 2, 9)]],
    subtitles=[dict(playlist=playlist, mode='off') for playlist in (1, 2)])
settings = dict(programJson=json.dumps(program), _programPropertiesTest=True)
plugin = root / 'dist/OBS-BluRay-0.2.0/obs-bluray'
env = os.environ.copy()
env['PATH'] = f'{obs}/bin/64bit;C:/Windows/System32;C:/Windows'
env['STREAMDESK_DATA'] = str(art/'data')
with (art/'native.log').open('wb') as log:
    result = subprocess.run([str(root/'build/obs-tests/obs-source-tests.exe'),
        str(plugin/'bin/64bit/obs-bluray.dll'), str(plugin/'data'), str(disc),
        str(art/'source'), '', 'software', '0', json.dumps(settings)],
        env=env, stdout=log, stderr=log, timeout=45, creationflags=subprocess.CREATE_NO_WINDOW)
assert result.returncode == 0, (art/'native.log').read_text(errors='replace')[-2500:]
print((art/'source.properties.json').read_text())
