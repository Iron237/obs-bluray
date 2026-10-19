"""Install isolated, pinned Windows SDKs; no system PATH changes or media edits."""
import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import requests

parser=argparse.ArgumentParser()
parser.add_argument('--sdk',default=str(Path(os.environ['LOCALAPPDATA'])/'StreamDeskSDK'))
args=parser.parse_args()
root=Path(args.sdk).resolve()
downloads=root/'downloads'
downloads.mkdir(parents=True,exist_ok=True)

def run(command):
    subprocess.run(list(map(str,command)),check=True,creationflags=subprocess.CREATE_NO_WINDOW)

def download(url,name=None):
    target=downloads/(name or url.rsplit('/',1)[-1])
    if not target.exists():
        with requests.get(url,stream=True,timeout=120) as response:
            response.raise_for_status()
            with target.open('wb') as out:
                for part in response.iter_content(1024*1024):out.write(part)
    return target

run([sys.executable,'-m','pip','install','aqtinstall==3.3.0','meson==1.12.1','ninja==1.13.2','psutil'])
qt=root/'Qt'
if not (qt/'6.8.3/msvc2022_64/bin/qmake.exe').exists():
    run([sys.executable,'-m','aqt','install-qt','windows','desktop','6.8.3','win64_msvc2022_64','-O',qt,'--archives','qtbase','qttools','qtsvg'])

base='https://gstreamer.freedesktop.org/data/pkg/windows/1.26.10/msvc/'
def setup_gst(kind):
    name=f'gstreamer-1.0-{kind}msvc-x86_64-1.26.10.msi'
    archive=download(base+name)
    response=requests.get(base+name+'.sha256sum',timeout=30);response.raise_for_status()
    expected=response.text.split()[0].lower()
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=expected:raise RuntimeError('SDK checksum mismatch: '+name)
    staging=root/('gst-devel' if kind else 'gst-runtime')
    if not (staging/'PFiles64/gstreamer/1.0/msvc_x86_64/bin').exists():
        run(['msiexec.exe','/a',archive,'/qn',f'TARGETDIR={staging}'])
    return staging/'PFiles64/gstreamer/1.0/msvc_x86_64'

# Administrative extraction is intentionally sequential: Windows Installer serializes it.
for kind in ('','devel-'):
    shutil.copytree(setup_gst(kind),root/'gstreamer',dirs_exist_ok=True)

source=root/'libbluray-1.4.0'
if not source.exists():
    archive=download('https://download.videolan.org/pub/videolan/libbluray/1.4.0/libbluray-1.4.0.tar.xz')
    with tarfile.open(archive) as tf:
        for member in tf.getmembers():
            if not (root/member.name).resolve().is_relative_to(root):raise RuntimeError('Unsafe archive member')
        tf.extractall(root)

header=source/'contrib/libudfread/src/udfread.h'
text=header.read_text(encoding='utf-8')
if 'typedef intptr_t ssize_t;' not in text:
    header.write_text(text.replace('#include <sys/types.h> /* *size_t */','#include <sys/types.h> /* *size_t */\n#ifdef _MSC_VER\ntypedef intptr_t ssize_t;\n#endif'),encoding='utf-8')
block=source/'contrib/libudfread/src/default_blockinput.c'
text=block.read_text(encoding='utf-8')
if 'typedef intptr_t ssize_t;' not in text:
    block.write_text(text.replace('#include <stdlib.h>','#include <stdlib.h>\n#ifdef _MSC_VER\n#include <stdint.h>\ntypedef intptr_t ssize_t;\n#endif'),encoding='utf-8')
build=root/'libbluray-build'
if not (build/'build.ninja').exists():
    run([sys.executable,'-m','mesonbuild.mesonmain','setup',build,source,'--vsenv','-Dbdj_jar=disabled','-Dfreetype=disabled','-Dlibxml2=disabled','-Dfontconfig=disabled','-Denable_tools=false','-Ddefault_library=shared','--buildtype=release'])
run([sys.executable,'-m','mesonbuild.mesonmain','compile','-C',build])
print('SDK ready:',root)
