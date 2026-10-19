# Validation: 0.2.1

Windows x64 / OBS 32.2.2, Qt 6.8.3, GStreamer 1.26.10 and MediaInfo 26.10. Tests generate synthetic media and XML; original media, paths, runtime configurations and capture logs remain local and ignored. The running OBS installation has not been replaced.

0.2.1 changes the separate program editor's appearance initialization and styles. All six suites were rerun successfully, with 16 editor checks and additional light/dark renders at 150% and 200% scale. Before the fix, the dark-palette replay rendered ordinary buttons and unselected table text white on white (1:1 contrast). After the fix these cases pass. The media/hardware/SRT runtime measurements below are retained from 0.2.0; playback behavior is unchanged by this UI correction.

## Automated checks

All six CTest suites pass with synthetic audio fixtures and the optional GUI screenshot enabled:

- Core: 10 QtTest checks for LPCM, validation, quoting, timestamp rebasing and configuration handling.
- Program model: 31 checks, including invalid-schema data rows, legacy compatibility, enabled ordering, single/sequence/loop navigation, MPLS ranges and subtitle configuration.
- Editor: 16 checks for range parsing, ordering, chapter candidates/names, per-MPLS subtitles, atomic save, cancel, navigation-title isolation, embedded SVG loading and synthetic screenshots. Light/dark system-palette regression cases reproduce the former white-on-white table/button text, then verify at least 4.5:1 text contrast after the standalone application's appearance initialization. The complete light palette also covers popup lists, inactive selections, disabled controls and tooltips; Fusion avoids mixing native dark controls with light surfaces.
- Audio details: 14 checks for TS/M2TS, LPCM sample rate/channels/depth, 4.608/2.304 Mbps calculations, malformed/partial headers and the 4 MiB limit.
- Compressed-audio probe: 16 checks with no skips. Real MediaInfo buffer parsing identifies synthetic AC-3 5.1/48 kHz/640 kbps and E-AC-3 stereo/48 kHz/384 kbps by PID. Mock reports verify Atmos/JOC evidence, ambiguity rejection and CBR versus prefix-estimate handling. No genuine Atmos disc sample was available, so actual Atmos-disc detection is not claimed as tested.
- Optional BDMV XML: 12 checks for DL/TN names, language preference, malformed files, namespace handling, bounded input, DTD/entity rejection, partial chapter-name matching and no invented playlist/time mapping.

## Runtime integration

- Native libobs: 4/4 hardware-enabled integration cases pass: CPU and Intel H.264, NVIDIA HEVC, separate transparent ASS, invalid input, pause/seek/resume/stop/restart and teardown. Audio conversion checks pass without clipping.
- Native worker program integration: 4/4 pass. A generated BDMV has two MPLS playlists and imprecise seek entry points. Three requested ranges produce 48 video frames and 96,000 audio sample frames each; independent audio-content matching locates their starts at 1, 5 and 9 seconds with correlation approximately 1.0. ASS follows the first two absolute MPLS ranges and is absent from the third. Controls cover paused-seek preview with silent audio, next/previous while paused, stop/resume/restart, single EOS, single loop and sequence wrap while skipping disabled entries.
- Native OBS properties: verifies saved program JSON survives initialization callback order, enabled episode choices, mode/selection edits, return to legacy controls, detailed LPCM labels and deferred draft editing. The editor does not restart the playing worker when opened or imported; normal OBS Apply/Cancel controls deployment of settings.
- Actual source read-only inspection also confirmed an LPCM track's language, 96 kHz, two channels, 24 bits, 4.608 Mbps, PID and PES ID. The private inspection output is excluded from Git.

## A/V synchronization regression

The 0.1.1 policy is retained on both Blu-ray and associated ASS sources: use unbuffered OBS async presentation for frames already paced by a shared producer QPC clock. It changes no audio offsets or producer timestamps.

The isolated 23.976-to-60 fps synthetic producer previously demonstrated a 900 ms video-only delivery gap becoming persistent 947-963 ms video delay under buffered presentation. Only enabling unbuffered presentation reduced the observed offsets to -3..30 ms. A deliberate 400 ms audio-delay negative control was rejected.

The final 0.2.0 encoded check uses two NVIDIA decoding sources, separate ASS, 1080p/23.976 H.264 plus 96 kHz/24-bit LPCM input, OBS 1080p60 composition, NVENC HEVC VBR 6000/7000 kbps, P5, two B frames, eight-frame lookahead, GOP 250 and AAC 320 kbps. A loopback receiver saves actual MPEG-TS SRT payload bytes without a second muxer. Seven flash/pulse pairs measure composed offsets -5.5..27.9 ms and encoded offsets -5.4..28.1 ms. Raw and encoded limits pass. Source-frame quantization applies; extra decimal precision is not a physical accuracy claim.

## Packaging and scope

The standalone package includes private Qt platform/SVG plugins, MediaInfo and notices, original SVG/ICO assets, and a binary SHA-256 manifest. SVG is compiled into the GUI resource and ICO into its Windows executable; neither reuses the OBS logo. Icon generation is reproducible from the committed SVG using Pillow.

The program editor is a separate process; Qt Widgets, MediaInfo and media libraries are not added to the OBS module. The metadata probe reads at most 4 MiB per distinct clip/audio description and does not decode or scan the entire stream. CBR uses parsed values; VBR estimates are explicitly marked as prefix estimates rather than whole-program averages.

Chapter marks are navigation points, not a guaranteed episode/song catalog. Optional XML names are used where present; full HDMV/BD-J menus are not executed. Switching episodes rebuilds the decoder and is not advertised as gapless. Internal PGS is composed into video; external ASS can use the linked transparent source. The input-to-OBS audio format remains 48 kHz stereo.

Hours-long playback of the original BDMV, remote multi-viewer behavior after deployment, real Atmos-disc samples, GPU zero-copy, HDR tone mapping, menus and BD-J are outside these checks. The earlier live incident's exact interruption is not established merely by reproducing the queue mechanism.
