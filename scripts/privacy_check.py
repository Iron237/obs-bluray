"""Public-source guard; reports locations/categories, never matched secrets."""
from pathlib import Path
import ipaddress
import re
import subprocess
import sys
from urllib.parse import urlsplit

ROOT=Path(__file__).resolve().parents[1]
EXCLUDED={'.git','build','dist','test-artifacts','sdk','downloads','__pycache__'}
OFFICIAL={'github.com','githubusercontent.com','qt.io','qt-project.org','videolan.org','freedesktop.org','gnu.org','fsf.org','ffmpeg.org','mediamtx.org','python.org','pypi.org'}
PATTERNS={
    'credential':re.compile(r'gh[pousr]_[A-Za-z0-9_]{25,}|github_pat_[A-Za-z0-9_]{25,}|sk-[A-Za-z0-9_-]{30,}|AKIA[A-Z0-9]{16}|AIza[A-Za-z0-9_-]{30,}'),
    'private-key':re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    'personal-path':re.compile(r'(?i)(?<![a-z0-9_])[a-z]:[/\\](?!Windows\b)[^\s"\x27<>;]*'),
    'private-ip':re.compile(r'(?<![\d.])(?:10\.\d+\.\d+\.\d+|192\.168\.\d+\.\d+|172\.(?:1[6-9]|2\d|3[01])\.\d+\.\d+)(?![\d.])'),
}
BLOCKED={'.env','live-server.config','live-server.runtime.yml','web-integration.json','server-stats.json'}

def paths():
    result=subprocess.run(['git','-C',str(ROOT),'ls-files','-z'],capture_output=True)
    if result.returncode==0 and result.stdout:
        return [ROOT/p for p in result.stdout.decode('utf-8').split('\0') if p]
    return [p for p in ROOT.rglob('*') if p.is_file() and not any(part in EXCLUDED or part.startswith('build-') for part in p.relative_to(ROOT).parts)]

def permitted(host):
    if not host:return True
    if host in {'localhost','host','example','example.com','example.org','example.net'} or host.endswith(('.example','.invalid','.test','.example.com','.example.org','.example.net')):return True
    try:return ipaddress.ip_address(host).is_loopback
    except ValueError:pass
    return any(host==domain or host.endswith('.'+domain) for domain in OFFICIAL)

def main():
    findings=[];files=paths()
    for path in files:
        relative=path.relative_to(ROOT).as_posix()
        if not path.resolve().is_relative_to(ROOT):findings.append((relative,0,'outside-root'));continue
        if path.name in BLOCKED or path.suffix.lower() in {'.log','.mkv','.mp4','.m2ts','.iso','.f32','.dll','.exe','.pdb','.config'}:
            findings.append((relative,0,'runtime-or-media'));continue
        if path.suffix.lower()=='.png':
            # Screenshots may be included; text metadata must be reviewed first.
            if any(tag in path.read_bytes() for tag in (b'tEXt',b'zTXt',b'iTXt')):findings.append((relative,0,'image-metadata'))
            continue
        try:text=path.read_text(encoding='utf-8-sig')
        except (UnicodeError,OSError):findings.append((relative,0,'unreviewed-binary'));continue
        for n,line in enumerate(text.splitlines(),1):
            for category,pattern in PATTERNS.items():
                if pattern.search(line):findings.append((relative,n,category))
            for match in re.finditer(r'(?:https?|rtmps?|rtsp|srt|udp|tcp)://[^\s"\x27<>`]+',line):
                address=match.group(0).rstrip(').,;')
                try:host=urlsplit(address).hostname
                except ValueError:continue
                # Ignore interpolated URL expressions, not concrete hostnames.
                if host and re.fullmatch(r'[a-zA-Z0-9.-]+',host) and not permitted(host.lower()):findings.append((relative,n,'unexpected-url-domain'))
            for match in re.finditer(r'(?i)(?<![a-z0-9_.-])(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+(?:com|org|net|cn|io|dev|app|tv|me|cloud|xyz|top|site|online|link|live|cc|co|work|info|biz)(?![a-z0-9_.-])',line):
                if not permitted(match.group(0).lower()):findings.append((relative,n,'unexpected-domain'))
    for name,line,category in sorted(set(findings)):
        print(f'{name}:{line}: {category}')
    print(f'Checked {len(files)} source files; {len(set(findings))} finding(s).')
    return bool(findings)

if __name__=='__main__':raise SystemExit(main())
