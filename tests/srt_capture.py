"""Test-only loopback SRT receiver preserving MPEG-TS payload bytes verbatim."""
import ctypes as c
import os
from pathlib import Path
import socket
import threading
import time


class Address(c.Structure):
    _fields_ = [('family', c.c_ushort), ('port', c.c_ushort),
                ('address', c.c_uint32), ('zero', c.c_ubyte * 8)]


class SrtCapture:
    def __init__(self, obs_root, path):
        directory = Path(obs_root) / 'bin/64bit'
        self.directory = os.add_dll_directory(str(directory))
        self.lib = c.CDLL(str(directory / 'srt.dll'))
        signatures = {
            'srt_startup': (c.c_int, []), 'srt_cleanup': (c.c_int, []),
            'srt_create_socket': (c.c_int, []),
            'srt_setsockflag': (c.c_int, [c.c_int, c.c_int, c.c_void_p, c.c_int]),
            'srt_bind': (c.c_int, [c.c_int, c.c_void_p, c.c_int]),
            'srt_getsockname': (c.c_int, [c.c_int, c.c_void_p, c.POINTER(c.c_int)]),
            'srt_listen': (c.c_int, [c.c_int, c.c_int]),
            'srt_accept': (c.c_int, [c.c_int, c.c_void_p, c.c_void_p]),
            'srt_recvmsg': (c.c_int, [c.c_int, c.c_void_p, c.c_int]),
            'srt_getlasterror': (c.c_int, [c.c_void_p]),
            'srt_getlasterror_str': (c.c_char_p, []),
            'srt_close': (c.c_int, [c.c_int]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.lib, name)
            function.restype, function.argtypes = result, arguments
        assert self.lib.srt_startup() == 0, 'SRT startup failed'
        self.listener = self.lib.srt_create_socket()
        assert self.listener != -1, 'SRT socket failed'
        # SRTO_TRANSTYPE=50 / SRTT_LIVE=0, from libsrt's public srt.h.
        live = c.c_int(0)
        assert self.lib.srt_setsockflag(self.listener, 50, c.byref(live), c.sizeof(live)) == 0
        address = Address(2, 0, int.from_bytes(socket.inet_aton('127.0.0.1'), 'little'))
        assert self.lib.srt_bind(self.listener, c.byref(address), c.sizeof(address)) == 0
        size = c.c_int(c.sizeof(address))
        assert self.lib.srt_getsockname(self.listener, c.byref(address), c.byref(size)) == 0
        self.port = socket.ntohs(address.port)
        assert self.lib.srt_listen(self.listener, 1) == 0
        self.path, self.client, self.bytes, self.error = Path(path), -1, 0, None
        self.stopping = False
        self.thread = threading.Thread(target=self._receive, daemon=True)

    def _receive(self):
        try:
            self.client = self.lib.srt_accept(self.listener, None, None)
            assert self.client != -1, 'SRT accept failed'
            timeout = c.c_int(2000)  # SRTO_RCVTIMEO=14.
            self.lib.srt_setsockflag(self.client, 14, c.byref(timeout), c.sizeof(timeout))
            buffer = c.create_string_buffer(65536)
            deadline = time.monotonic() + 180
            with self.path.open('wb') as stream:
                while not self.stopping and time.monotonic() < deadline:
                    count = self.lib.srt_recvmsg(self.client, buffer, len(buffer))
                    if count < 0:
                        # SRT_ETIMEOUT=6003; a closed peer ends the capture.
                        if self.lib.srt_getlasterror(None) == 6003:
                            continue
                        break
                    if not count:
                        break
                    stream.write(buffer.raw[:count])
                    self.bytes += count
        except Exception as error:
            self.error = str(error)
        finally:
            if self.client != -1:
                self.lib.srt_close(self.client)
                self.client = -1

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, kind, value, traceback):
        self.thread.join(3)
        self.stopping = True
        self.lib.srt_close(self.listener)
        self.thread.join(3)
        assert not self.thread.is_alive(), 'SRT receiver did not stop'
        self.lib.srt_cleanup()
        self.directory.close()
        if kind is None:
            assert self.error is None, self.error
            assert self.bytes > 188 and self.bytes % 188 == 0, 'Incomplete MPEG-TS capture'
