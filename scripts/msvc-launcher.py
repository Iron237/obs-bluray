"""Normalize localized MSVC /showIncludes output for Ninja dependency tracking.

The installed Chinese-only toolchain emits CP936 even with VSLANG=1033.
CMake's guessed prefix can be double-decoded, silently losing all header deps.
Keep compiler return codes and stream diagnostics; use one stable UTF-8 prefix.
"""
import locale
import ctypes
import subprocess
import sys


def normalize(line: bytes) -> bytes:
    try:
        text = line.decode('utf-8')
    except UnicodeDecodeError:
        encoding = 'cp' + str(ctypes.windll.kernel32.GetACP()) if sys.platform == 'win32' else locale.getpreferredencoding(False)
        text = line.decode(encoding, errors='replace')
    prefix = '注意: 包含文件:'
    if text.startswith(prefix):
        text = 'Note: including file:' + text[len(prefix):]
    return text.encode('utf-8')


if __name__ == '__main__':
    process = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for line in process.stdout:
        sys.stdout.buffer.write(normalize(line))
        sys.stdout.buffer.flush()
    raise SystemExit(process.wait())
