#!/usr/bin/env python3
"""Convert one local video to the Fruit Jam Retro TV's bounded baseline MJPEG + PCM format.

Default output is 320x240 (the full logical frame, doubled to 640x480 on DVI).
--size 240x160 keeps the smaller upstream (m5-retro-tv) default, cheaper to
decode and entirely inside a CRT's safe area when watched through an HDMI->AV
converter. The same card folder plays on both devices.
"""
import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path, help='New program directory (must not exist)')
    parser.add_argument('--size', choices=('320x240', '240x160'), default='320x240',
                        help='Frame size (default 320x240, full screen; 240x160 decodes faster)')
    parser.add_argument('--fps', type=int, choices=range(10, 31), default=15)
    parser.add_argument('--quality', type=int, choices=range(2, 16), default=5, help='JPEG qscale, lower is better/larger (default 5)')
    parser.add_argument('--title')
    args = parser.parse_args()
    for tool in ('ffmpeg', 'ffprobe'):
        if not shutil.which(tool):
            parser.error(f'{tool} is required on PATH')
    source = args.source.resolve()
    destination = args.destination.resolve()
    if not source.is_file():
        parser.error('Source file does not exist')
    if destination.exists():
        parser.error('Destination already exists; choose a new directory')
    probe = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(source)
    ], text=True))
    streams = probe['streams']
    videos = [s for s in streams if s['codec_type'] == 'video' and not s.get('disposition', {}).get('attached_pic')]
    if not videos:
        parser.error('Source has no video stream')
    duration = float(videos[0].get('duration') or probe.get('format', {}).get('duration') or 0)
    if not math.isfinite(duration) or duration <= 0:
        parser.error('Cannot determine video duration')
    # Align the duration to complete video frames; pad short/missing audio with silence.
    frames = math.ceil(duration * args.fps)
    duration = frames / args.fps
    width, height = map(int, args.size.split('x'))
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.retrotv-convert-', dir=destination.parent) as tmp:
        folder = Path(tmp)
        command = ['ffmpeg', '-nostdin', '-v', 'error', '-i', str(source)]
        has_audio = any(s['codec_type'] == 'audio' for s in streams)
        if not has_audio:
            command += ['-f', 'lavfi', '-i', 'anullsrc=r=22050:cl=stereo']
        command += [
            '-map', f'0:{videos[0]["index"]}', '-an',
            '-vf', f'setpts=PTS-STARTPTS,fps={args.fps},scale={width}:{height}:force_original_aspect_ratio=decrease,pad={width}:{height}:(ow-iw)/2:(oh-ih)/2,setsar=1,tpad=stop_mode=clone:stop_duration=1',
            '-frames:v', str(frames), '-c:v', 'mjpeg', '-q:v', str(args.quality),
            '-pix_fmt', 'yuvj420p', '-map_metadata', '-1', '-f', 'mjpeg', str(folder / 'video.mjpeg'),
            '-map', '0:a:0' if has_audio else '1:a:0', '-vn',
            '-af', 'asetpts=PTS-STARTPTS,apad', '-t', str(duration),
            '-ar', '22050', '-ac', '2', '-c:a', 'pcm_s16le', '-map_metadata', '-1', str(folder / 'audio.wav')
        ]
        subprocess.run(command, check=True)
        # Verify actual JPEG frame sizes before producing a card-ready package.
        count, maximum, pending = 0, 0, bytearray()
        with (folder / 'video.mjpeg').open('rb') as file:
            while chunk := file.read(65536):
                pending.extend(chunk)
                while (end := pending.find(b'\xff\xd9')) >= 0:
                    frame = pending[:end+2]
                    if not frame.startswith(b'\xff\xd8'):
                        raise RuntimeError('Invalid MJPEG frame')
                    maximum = max(maximum, len(frame))
                    count += 1
                    del pending[:end+2]
                if len(pending) > 128*1024:
                    raise RuntimeError('JPEG exceeds 128 KiB; increase --quality or reduce --size')
        if pending or count != frames or maximum > 128*1024:
            raise RuntimeError('MJPEG validation failed: frame count or size')
        (folder / 'meta.json').write_text(json.dumps({
            'title': args.title or source.stem, 'fps': args.fps,
            'video': 'video.mjpeg', 'audio': 'audio.wav',
            'width': width, 'height': height, 'frames': frames,
        }, ensure_ascii=False, indent=2) + '\n')
        # All validation succeeded. Rename on the same filesystem.
        folder.rename(destination)
    print(f'Created {destination}: {frames} frames, {duration:.3f}s, largest JPEG {maximum} bytes')


if __name__ == '__main__':
    main()
