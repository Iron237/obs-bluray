"""Measure composed OBS video flashes against mixed-audio pulses on OBS clocks.

Requires a local OBS installation and built native test host. No live OBS session,
server, user media or configuration is opened or changed.
"""
import argparse
import csv
import contextlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import re

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
PULSES = [2, 4.7, 7.1, 10.4, 12.3, 15.7, 18.2, 21.6, 24, 27.9,
          30.2, 33.6, 36.1, 39.9, 42.2, 45.7, 48.4, 51.8, 54.1, 57.9,
          60.2, 63.6, 66.1, 69.9, 72.2, 75.7, 78.4, 81.8, 84.1, 87.9,
          90.2, 93.6, 96.1, 99.9, 102.2, 105.7, 108.4, 111.8, 114.1, 117.9]


def read_csv(path):
    with path.open(encoding='utf-8') as stream:
        return list(csv.DictReader(stream))


def measure(prefix, gap_ms=0):
    frames = read_csv(prefix.with_suffix('.rendered.csv'))
    assert frames, 'No composed OBS video frames captured'
    timestamps = np.array([int(x['timestamp_ns']) / 1e9 for x in frames])
    luma = np.array([float(x['luma']) for x in frames])
    assert luma.max() - luma.min() > 40, 'No rendered flash contrast'
    bright = luma > (luma.min() + luma.max()) / 2
    video_events = timestamps[np.flatnonzero(bright & ~np.r_[False, bright[:-1]])]

    samples = np.fromfile(prefix.with_suffix('.mix.f32'), dtype='<f4').reshape(-1, 2)
    assert np.isfinite(samples).all(), 'Non-finite mixed PCM'
    count = len(samples) // 48
    energy = np.sqrt(np.mean(samples[:count * 48, 0].reshape(-1, 48) ** 2, axis=1))
    audible = energy > .02
    sample_events = np.flatnonzero(audible & ~np.r_[False, audible[:-1]]) * 48
    blocks = read_csv(prefix.with_suffix('.audio.csv'))
    starts = np.array([int(x['first_frame']) for x in blocks])
    clock = np.array([int(x['timestamp_ns']) / 1e9 for x in blocks])
    ids = np.searchsorted(starts, sample_events, side='right') - 1
    assert (ids >= 0).all(), 'Pulse outside captured audio blocks'
    audio_events = clock[ids] + (sample_events - starts[ids]) / 48000

    # The fixture uses unequal pulse intervals. Pairing all events by ordinal
    # avoids silently accepting a one-pulse shift through nearest-neighbor fit.
    assert len(audio_events) == len(video_events), {
        'audioPulses': len(audio_events), 'videoPulses': len(video_events),
        'audioTimes': audio_events.tolist(), 'videoTimes': video_events.tolist(),
    }
    assert len(video_events) >= 7, 'Not enough independently observed flash/pulse events'
    offsets = video_events - audio_events
    source_frames = read_csv(prefix.with_suffix('.source.csv'))
    source_luma = np.array([float(x['luma']) for x in source_frames])
    source_bright = source_luma > (source_luma.min() + source_luma.max()) / 2
    source_timestamps = np.array([int(x['timestamp_ns']) / 1e9 for x in source_frames])
    source_events = source_timestamps[np.flatnonzero(source_bright & ~np.r_[False, source_bright[:-1]])]
    assert len(source_events) == len(audio_events), 'Missing raw-source flash/pulse events'
    # A positive offset means OBS renders the flash after its matching audio.
    result = dict(audioPulses=len(audio_events), videoPulses=len(video_events),
                videoMinusAudioMs=(offsets * 1000).tolist(),
                medianVideoMinusAudioMs=float(np.median(offsets) * 1000),
                maxAbsoluteOffsetMs=float(np.max(np.abs(offsets)) * 1000),
                firstToLastDriftMs=float((offsets[-1] - offsets[0]) * 1000),
                sourceVideoMinusAudioMs=((source_events - audio_events) * 1000).tolist(),
                medianPresentationBufferMs=float(np.median(video_events - source_events) * 1000),
                observedSeconds=float(video_events[-1] - video_events[0]))
    if gap_ms:
        media_time = audio_events - audio_events[0] + PULSES[0]
        before = offsets[media_time < 8]
        after = offsets[media_time > 8 + gap_ms / 1000]
        assert len(before) >= 3 and len(after) >= 3, 'Insufficient events around delivery gap'
        result.update(preGapEvents=len(before), postGapEvents=len(after),
                      preGapMedianMs=float(np.median(before) * 1000),
                      postGapMedianMs=float(np.median(after) * 1000),
                      postMinusPreGapMs=float((np.median(after) - np.median(before)) * 1000))
    return result


def measure_encoded(ffmpeg, prefix):
    """Decode the original SRT payload using one unchanged MPEG-TS PTS domain."""
    hide = dict(creationflags=subprocess.CREATE_NO_WINDOW)
    captured = prefix.with_suffix('.encoded.ts')
    video_log = prefix.with_suffix('.encoded-video.log')
    with video_log.open('wb') as log:
        subprocess.run([ffmpeg, '-v', 'info', '-y', '-copyts', '-i', str(captured),
                        '-map', '0:v:0', '-an', '-vf', 'crop=8:8:iw/2-4:ih/2-4,format=gray,showinfo',
                        '-fps_mode', 'passthrough', '-f', 'rawvideo', str(prefix.with_suffix('.encoded.gray'))],
                       stdout=log, stderr=log, check=True, timeout=60, **hide)
    image = np.fromfile(prefix.with_suffix('.encoded.gray'), dtype='u1').reshape(-1, 64).mean(axis=1)
    text = video_log.read_text(encoding='utf-8', errors='replace')
    pts = np.array([float(x) for x in re.findall(r'\bn:\s*\d+\s+pts:\s*-?\d+\s+pts_time:\s*(-?[\d.]+)', text)])
    assert len(pts) == len(image), 'Decoded video frame/PTS count mismatch'
    assert image.max() - image.min() > 40, 'No encoded flash contrast'
    bright = image > (image.min() + image.max()) / 2
    video_events = pts[np.flatnonzero(bright & ~np.r_[False, bright[:-1]])]
    audio_log = prefix.with_suffix('.encoded-audio.log')
    with audio_log.open('wb') as log:
        subprocess.run([ffmpeg, '-v', 'info', '-y', '-copyts', '-i', str(captured), '-map', '0:a:0', '-vn',
                        '-af', 'aformat=sample_fmts=flt:sample_rates=48000:channel_layouts=stereo,ashowinfo',
                        '-f', 'f32le', str(prefix.with_suffix('.encoded.f32'))],
                       stdout=log, stderr=log, check=True, timeout=60, **hide)
    samples = np.fromfile(prefix.with_suffix('.encoded.f32'), dtype='<f4').reshape(-1, 2)
    text = audio_log.read_text(encoding='utf-8', errors='replace')
    blocks = [(float(t), int(n)) for t, n in re.findall(r'\bn:\d+\s+pts:\s*-?\d+\s+pts_time:\s*(-?[\d.]+).*?\bnb_samples:(\d+)', text)]
    assert sum(n for _, n in blocks) == len(samples), 'Decoded audio sample/PTS count mismatch'
    count = len(samples) // 48
    audible = np.sqrt(np.mean(samples[:count * 48, 0].reshape(-1, 48) ** 2, axis=1)) > .02
    sample_events = np.flatnonzero(audible & ~np.r_[False, audible[:-1]]) * 48
    starts = np.r_[0, np.cumsum([n for _, n in blocks])[:-1]]
    ids = np.searchsorted(starts, sample_events, side='right') - 1
    audio_events = np.array([t for t, _ in blocks])[ids] + (sample_events - starts[ids]) / 48000
    assert len(audio_events) == len(video_events) and len(audio_events) >= 7, {
        'encodedAudioEvents': audio_events.tolist(), 'encodedVideoEvents': video_events.tolist()}
    offsets = (video_events - audio_events) * 1000
    return dict(audioPulses=len(audio_events), videoPulses=len(video_events),
                videoMinusAudioMs=offsets.tolist(), medianVideoMinusAudioMs=float(np.median(offsets)),
                maxAbsoluteOffsetMs=float(np.max(np.abs(offsets))),
                firstToLastDriftMs=float(offsets[-1] - offsets[0]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=30, choices=range(20, 121))
    parser.add_argument('--decoder', choices=['software', 'intel', 'nvidia'], default='software')
    parser.add_argument('--fixture-format', choices=['mkv', 'm2ts'], default='mkv',
                        help='m2ts exercises Blu-ray LPCM at 96 kHz/24-bit through the transport demuxer')
    parser.add_argument('--second-source', action='store_true')
    parser.add_argument('--synthetic', action='store_true', help='Use a test-only QPC source without a decoder')
    parser.add_argument('--video-gap-ms', type=int, default=0,
                        help='With --synthetic only, skip video delivery at source 8 s while audio/clock continue')
    parser.add_argument('--unbuffered', action='store_true', help='Test-only OBS async unbuffered flag; no production defaults change')
    parser.add_argument('--allow-buffered-legacy', action='store_true', help='Permit pre-fix plugin modules during comparative diagnostics')
    parser.add_argument('--full-hd', action='store_true')
    parser.add_argument('--encode-srt', action='store_true',
                        help='Also test real OBS NVENC HEVC/AAC -> MPEGTS -> verbatim loopback SRT payload; implies Full HD')
    parser.add_argument('--fixture-audio-delay-ms', type=int, default=0,
                        help='Measurement negative control; a nonzero delay should fail the sync limits')
    parser.add_argument('--module', type=Path, default=ROOT / 'dist/OBS-BluRay-0.2.0/obs-bluray/bin/64bit/obs-bluray.dll')
    parser.add_argument('--data', type=Path, default=ROOT / 'dist/OBS-BluRay-0.2.0/obs-bluray/data')
    parser.add_argument('--name', default='sync')
    args = parser.parse_args()
    if args.synthetic and args.fixture_audio_delay_ms:
        parser.error('Synthetic source does not support --fixture-audio-delay-ms')
    if args.synthetic and args.second_source:
        parser.error('Synthetic gap isolation uses exactly one source')
    if args.video_gap_ms and not args.synthetic:
        parser.error('A delivery gap requires --synthetic')
    if not 0 <= args.video_gap_ms <= 900:
        parser.error('Supported delivery gaps are 0..900 ms; longer gaps could overlap a flash cue')
    ffmpeg = os.environ.get('STREAMDESK_FFMPEG') or shutil.which('ffmpeg')
    assert ffmpeg, 'Set STREAMDESK_FFMPEG'
    assert os.environ.get('OBS_ROOT'), 'Set OBS_ROOT'
    art = ROOT / 'test-artifacts/synchronization'
    art.mkdir(parents=True, exist_ok=True)
    prefix = art / args.name
    fixture = art / ('flash-pulse-' + args.name + '.' + args.fixture_format)
    enabled = '+'.join(f'between(t,{t},{t + .125})' for t in PULSES)
    audio_enabled = '+'.join(f'between(t,{t + args.fixture_audio_delay_ms / 1000},{t + .125 + args.fixture_audio_delay_ms / 1000})' for t in PULSES)
    hide = dict(creationflags=subprocess.CREATE_NO_WINDOW)
    audio_codec = ['-c:a', 'pcm_bluray', '-ar', '96000', '-ac', '2', '-sample_fmt', 's32'] if args.fixture_format == 'm2ts' else ['-c:a', 'pcm_s24le']
    full_hd = args.full_hd or args.encode_srt
    dimensions = '1920x1080' if full_hd else '640x360'
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i',
                    f"color=black:s={dimensions}:r=24000/1001,drawbox=color=white:t=fill:enable='{enabled}'",
                    '-f', 'lavfi', '-i', f"aevalsrc='if(gt({audio_enabled},0),0.25*sin(2*PI*1000*t),0)':s=48000",
                    '-t', str(args.seconds + 12), '-c:v', 'libx264', '-preset', 'ultrafast',
                    *audio_codec, str(fixture)], check=True, timeout=45, **hide)
    subtitle = art / 'test.ass'
    subtitle.write_text((ROOT / 'tests/fixtures/obs.ass').read_text(encoding='utf-8'), encoding='utf-8')
    env = os.environ.copy()
    env['PATH'] = f"{env['OBS_ROOT']}/bin/64bit;C:/Windows/System32;C:/Windows"
    env['STREAMDESK_DATA'] = str(art / ('data-' + args.name))
    settings = dict(_syncTestSeconds=args.seconds, _syncSecondSource=args.second_source, _syncFullHD=full_hd,
                    _syncSynthetic=args.synthetic, _syncGapMs=args.video_gap_ms, _syncUnbuffered=args.unbuffered)
    settings['_allowBufferedLegacy'] = args.allow_buffered_legacy
    if args.encode_srt:
        from srt_capture import SrtCapture
        # OBS locates this helper relative to its host executable. Copy it into
        # our ignored test build directory, leaving the OBS installation intact.
        shutil.copy2(Path(env['OBS_ROOT']) / 'bin/64bit/obs-nvenc-test.exe',
                     ROOT / 'build/obs-tests/obs-nvenc-test.exe')
        receiver = SrtCapture(env['OBS_ROOT'], prefix.with_suffix('.encoded.ts'))
    else:
        receiver = contextlib.nullcontext()
    with receiver as capture:
        if capture is not None:
            settings['_syncEncodeUrl'] = f'srt://127.0.0.1:{capture.port}?mode=caller&latency=120000'
        with prefix.with_suffix('.log').open('wb') as log:
            subprocess.run([str(ROOT / 'build/obs-tests/obs-source-tests.exe'), str(args.module), str(args.data),
                            str(fixture), str(prefix), '' if args.synthetic else str(subtitle), args.decoder, '0', json.dumps(settings)],
                           stdout=log, stderr=log, env=env, check=True, timeout=args.seconds + 90, **hide)
    result = dict(decoder=args.decoder, independentSources=2 if args.second_source else 1,
                  fixtureAudioDelayMs=args.fixture_audio_delay_ms,
                  fixtureFormat=args.fixture_format,
                  synthetic=args.synthetic, videoGapMs=args.video_gap_ms,
                  unbuffered=args.unbuffered or (not args.synthetic and not args.allow_buffered_legacy),
                  separateAss=not args.synthetic, outputFps=60, dimensions=dimensions, sourceFps='24000/1001', **measure(prefix, args.video_gap_ms))
    if args.encode_srt:
        result['encodedSrt'] = measure_encoded(ffmpeg, prefix)
    prefix.with_suffix('.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
    # 23.976 fps flash quantization (42 ms), OBS asynchronous source buffering
    # (normally about two source frames) and output frame quantization all apply.
    assert result['maxAbsoluteOffsetMs'] < 160, 'Rendered A/V offset exceeded 160 ms'
    assert abs(result['firstToLastDriftMs']) < 80, 'Rendered A/V drift exceeded 80 ms'
    if args.encode_srt:
        assert result['encodedSrt']['maxAbsoluteOffsetMs'] < 160, 'Encoded A/V offset exceeded 160 ms'
        assert abs(result['encodedSrt']['firstToLastDriftMs']) < 80, 'Encoded A/V drift exceeded 80 ms'


if __name__ == '__main__':
    main()
