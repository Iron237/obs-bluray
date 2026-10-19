#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <cstring>

namespace sdobs {
constexpr uint32_t magic = 0x53444F31;
constexpr size_t maxVideo = 4096ULL * 2160 * 4;
constexpr uint32_t audioSlots = 64, audioBytes = 65536;
enum State : uint32_t { opening, playing, paused, stopped, ended, failed };
enum Command : uint32_t { none, pause, resume, seek, stop, restart };
struct Audio { uint64_t timestamp; uint32_t bytes; uint8_t data[audioBytes]; };
struct Shared {
    uint32_t signature, version;
    uint64_t videoSequence, videoTimestamp;
    uint32_t width, height, stride, videoBytes;
    uint64_t audioWrite, audioRead, audioDropped;
    uint64_t commandSequence;
    uint32_t command, state;
    int64_t commandValue, positionMs, durationMs;
    uint64_t subtitleFrames;
    uint32_t separateSubtitles;
    char error[1024];
    uint8_t video[maxVideo];
    uint8_t subtitle[maxVideo];
    Audio audio[audioSlots];
};
class Mapping {
    HANDLE mapping_ = nullptr, mutex_ = nullptr;
public:
    Shared *data = nullptr;
    bool open(const std::wstring &name, bool create) {
        mutex_ = CreateMutexW(nullptr, FALSE, (name + L"-lock").c_str());
        mapping_ = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(sizeof(Shared)), name.c_str())
                          : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if(mapping_) data = static_cast<Shared *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if(!data || !mutex_) return false;
        if(create) { data->signature = magic; data->version = 1; }
        return data->signature == magic && data->version == 1;
    }
    bool lock(DWORD timeout = 100) { auto r = WaitForSingleObject(mutex_, timeout); return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED; }
    void unlock() { ReleaseMutex(mutex_); }
    ~Mapping() { if(data) UnmapViewOfFile(data); if(mapping_) CloseHandle(mapping_); if(mutex_) CloseHandle(mutex_); }
};
inline uint64_t nowNs() { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return uint64_t(t.QuadPart / f.QuadPart) * 1000000000ULL + uint64_t(t.QuadPart % f.QuadPart) * 1000000000ULL / uint64_t(f.QuadPart); }
}
