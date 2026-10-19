# Validation: 0.1.1

Windows x64 / OBS 32.2.2, Qt 6.8.3 and GStreamer 1.26.10. Public tests generate synthetic fixtures; user media, names, paths, runtime configuration and raw diagnostic logs are excluded.

## Video-gap regression

The test-only QPC producer emits synchronized flash/pulse content at 23.976 fps and 48 kHz. OBS composes at 60 fps. At source time 8.0 seconds, only video delivery stops for 900 ms: audio delivery and both clocks continue, and resumed video retains its original timestamp. The test does not block the render thread or insert an audio offset. The gap does not overlap a flash cue.

| Policy | Before-gap median, video minus audio | After-gap median | Observations |
| --- | ---: | ---: | --- |
| OBS buffered async default | 30.009 ms | 963.342 ms | 3 events before, 4 after; fails offset/drift limits |
| Only `async_unbuffered=true` | -3.411 ms | 13.255 ms | 3 events before, 8 after; all offsets -3.412..29.922 ms |

This reproduces a persistent video backlog after delivery resumes, despite correct source timestamps. Version 0.1.1 enables the unbuffered presentation policy on both the Blu-ray and associated ASS source before their worker polling starts. It changes no timestamps, audio compensation or decoupled-audio policy.

Reproduce the negative control (expected nonzero exit), then the positive control:

```powershell
python tests/synchronization.py --seconds 20 --synthetic --video-gap-ms 900 --name gap-red
python tests/synchronization.py --seconds 30 --synthetic --video-gap-ms 900 --unbuffered --name gap-green
```

These are isolated synthetic-source tests of OBS presentation behavior. They are combined with the following tests of the actual plugin; they do not establish the exact interruption that triggered a user's earlier live incident.

## Actual 0.1.1 plugin regression

- Core: **10 passed, 0 failed**. Blu-ray LPCM 16/24-bit packing, validation, encryption/redaction, pipeline quoting, disc paths and transport timestamp rebasing.
- Native libobs integration: **4 passed, 0 failed**, hardware tests enabled. CPU/Intel H.264, NVIDIA HEVC, separate transparent ASS, invalid input, pause/seek/resume/stop/restart and source teardown. The host checks both actual plugin sources with `obs_source_async_unbuffered()`.
- Synthetic audio quality: correlation above 0.9999, gain-adjusted SNR above 126 dB and zero clipped samples. This uses a fixed one-second window after 500 ms warm-up; it is separate from the A/V timing test.
- A deliberately introduced 400 ms audio delay is rejected by the timing test. Source timing measures the injected delay within one source frame.
- 0.1.1 was rebuilt and packaged independently; package DLL hashes match the build outputs.

The final encoded run uses two actual NVIDIA decoding sources, separate ASS, 1920×1080 / 23.976 fps H.264 input and 96 kHz/24-bit Blu-ray LPCM in M2TS. OBS composes and encodes 1920×1080 / 60 fps using NVENC HEVC VBR 6000/7000 kbps, P5, two B frames, eight-frame lookahead, GOP 250 and AAC 320 kbps / 48 kHz. Its real MPEG-TS SRT output goes to a loopback receiver that saves payload bytes verbatim, without another muxer. Packet PTS/DTS/CTS are also observed after libobs interleaving and before muxing.

Over the 20-second run, composed output has 8 flash/pulse events with offsets -10.768..22.567 ms. The encoded SRT payload has 7 complete events with offsets -11.000..22.333 ms. The difference in counts is at the stopped output's capture boundary; medians over unequal event counts should not be treated as an added encoding delay. The raw and encoded timing limits both pass.

## Boundaries

The fix prevents the reproduced short video-only gap from becoming persistent OBS video backlog. The existing running OBS installation has not been replaced or retested as part of these isolated release checks. Original live-trigger attribution, hours of accumulated state, the original BDMV playlist, MediaMTX and remote-player recovery are outside this regression.

Original development separately exercised real BDMV playlists/chapters, PGS and LPCM. No claim of GPU zero-copy, full HDR, Blu-ray menus or BD-J support is made. Generated media, PCM, logs and machine-specific SDK paths remain local and ignored.
