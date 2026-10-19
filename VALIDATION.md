# Validation baseline

Version 0.1.0 targets Windows x64 / OBS 32.2.2. Validation covers real libobs module loading, source video/audio, independent transparent ASS output, pause/seek/resume/stop/restart, and worker cleanup.

Original development also exercised CPU/Intel/NVIDIA decoding, H.264/HEVC input, BDMV playlists/chapters, PGS and Blu-ray LPCM. User media, paths, names, raw PCM and diagnostic logs are excluded from the public repository. Public integration tests generate synthetic color/chirp/ASS fixtures; hardware paths are opt-in.

The independent source split was rebuilt and packaged on Windows x64 with Qt 6.8.3, GStreamer 1.26.10 and OBS 32.2.2:

- Core: **10 passed, 0 failed**. Covers Blu-ray LPCM 16/24-bit packing, configuration validation/encryption/redaction, pipeline quoting, disc paths and transport timestamp rebasing.
- Native libobs integration: **4 passed, 0 failed** with hardware tests enabled. Covers CPU and Intel H.264, NVIDIA HEVC, separate transparent ASS, invalid input, pause/seek/resume/stop/restart and source teardown.
- Synthetic audio checks: correlation above 0.9999, gain-adjusted SNR above 126 dB and zero clipped samples. Quality compares a fixed one-second window after a 500 ms warm-up against the synthetic reference; it does not measure absolute A/V offset.
- Standalone plugin packaging completed successfully. The public source privacy check passed with no findings; runtime state, generated fixtures and raw results remain excluded.

These are short synthetic source/control and conversion-quality tests. They do **not** verify long-running A/V drift, multi-viewer server behavior or end-to-end playback synchronization. No claim of zero-copy GPU transfer or complete HDR/menu/BD-J support is made.
