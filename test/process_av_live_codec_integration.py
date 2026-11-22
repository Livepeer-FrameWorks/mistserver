#!/usr/bin/env python3
"""Exercise actual live decoder input, local renditions and decoded output."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import urllib.request
from controller_capacity_lifecycle_integration import Fixture, until
from process_av_rotation_integration import verify_corner


def main():
    if os.environ.get('MIST_RUN_MEDIA_TESTS') != '1':
        return 77
    build, ffmpeg, ffprobe, codec = Path(sys.argv[1]).resolve(), *sys.argv[2:]
    video = codec in ('HEVC', 'MPEG2', 'MPEG4')
    fixture = Fixture(build / 'MistController', ffmpeg)
    fixture.stream = 'livecodec' + str(os.getpid())
    fixture.streams.add(fixture.stream)
    success = False
    try:
        source = fixture.root / 'source.mkv'
        if video:
            encoder = {'HEVC': ['libx265', '-preset', 'ultrafast', '-x265-params', 'pools=1:log-level=error'],
                       'MPEG2': ['mpeg2video'], 'MPEG4': ['mpeg4']}[codec]
            args = ['-f', 'lavfi', '-i', 'color=blue:size=320x180:rate=10:duration=12',
                    '-vf', 'drawbox=x=0:y=0:w=80:h=80:color=red:t=fill',
                    '-c:v', *encoder, '-threads', '1', '-pix_fmt', 'yuv420p', '-g', '10', '-bf', '2']
            processes = [{'process': 'AV', 'x-LSP-kind': 'video', 'codec': 'H264', 'resolution': '0x%d' % height,
                          'accel': 'sw', 'preset': 'ultrafast', 'tune': 'zerolatency', 'gopsize': 10,
                          'source_mask': 4, 'target_mask': 3, 'track_select': 'video=%s&audio=none' % codec}
                         for height in (90, 180)]
            processes.append({'process': 'Thumbs', 'track_select': 'video=' + codec,
                              'thumb_width': 80, 'thumb_height': 80, 'interval': 1000, 'target_mask': 3})
            query = 'video=H264&audio=none'
        else:
            channels = 6 if codec in ('AC3', 'EAC-3') else 2
            args = ['-f', 'lavfi', '-i', 'sine=frequency=701:sample_rate=48000:duration=12',
                    '-ac', str(channels), '-c:a', {'MP2': 'mp2', 'MP3': 'libmp3lame', 'AC3': 'ac3', 'EAC-3': 'eac3'}[codec], '-b:a', '384k']
            processes = [{'process': 'AV', 'x-LSP-kind': 'audio', 'codec': output,
                          'source_mask': 4, 'target_mask': 3, 'track_select': 'audio=%s&video=none' % codec,
                          'max_channels': 2 if output == 'opus' else 0, 'bitrate': 128000 if output == 'opus' else 384000}
                         for output in ('AAC', 'opus')]
            query = 'video=none&audio=all,!' + codec
        subprocess.run([ffmpeg, '-v', 'error', '-y', *args, str(source)], env=fixture.tool_env, check=True, timeout=60)
        command = shlex.join([ffmpeg, '-hide_banner', '-loglevel', 'error', '-re', '-stream_loop', '-1', '-i', str(source),
                              '-map', '0', '-c', 'copy', '-f', 'matroska', '-live', '1', '-'])
        fixture.start(streams_override={fixture.stream: {'source': 'mkv-exec:' + command, 'processes': processes}})
        recording = fixture.root / 'live.mkv'
        def live_metadata():
            try:
                with urllib.request.urlopen('http://127.0.0.1:%d/json_%s.js' % (fixture.http, fixture.stream), timeout=5) as response:
                    result = json.load(response)
            except OSError:
                return None
            meta = result.get('meta', {})
            tracks = list(meta.get('tracks', {}).values())
            observed = {(track.get('width'), track.get('height')) for track in tracks if track.get('codec') == 'H264'} if video else {(track.get('codec', '').lower(), track.get('channels')) for track in tracks}
            expected = {(160, 90), (320, 180)} if video else {('aac', channels), ('opus', 2)}
            if meta.get('live') and expected.issubset(observed):
                return result
            return None
        metadata = until(live_metadata, 20, 'pipe source did not publish live metadata')
        (fixture.root / 'live-metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
        output_log = (fixture.root / 'output.log').open('wb')
        output = subprocess.Popen([str(build / 'MistOutEBML'), '-s', fixture.stream,
                                   str(recording) + '?' + query + '&subtitle=none&meta=none&duration=8'],
                                  env=fixture.env, stdout=output_log, stderr=subprocess.STDOUT)
        fixture.children.append(output)
        assert output.wait(timeout=40) == 0, 'live recording failed'
        output_log.close()
        probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-show_streams', '-show_packets', '-of', 'json', str(recording)], env=fixture.tool_env))
        streams = probe['streams']
        if video:
            assert {(track['codec_name'], track['width'], track['height']) for track in streams} == {('h264', 160, 90), ('h264', 320, 180)}, streams
        else:
            assert {(track['codec_name'], track['channels']) for track in streams} == {('aac', channels), ('opus', 2)}, streams
        for index, track in enumerate(streams):
            packets = [packet for packet in probe['packets'] if packet['stream_index'] == track['index']]
            times = [float(packet['pts_time']) for packet in packets]
            assert times and times[-1] - times[0] >= 7.7, (track, times[:1], times[-1:])
            if video:
                verify_corner(ffmpeg, recording, index, track['width'], track['height'], 0)
                decoded = subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', str(track['index']), '-count_frames', '-show_entries', 'stream=nb_read_frames', '-of', 'csv=p=0', str(recording)], env=fixture.tool_env)
                assert int(decoded.strip()) >= 78, (track, decoded)
            else:
                pcm = subprocess.check_output([ffmpeg, '-v', 'error', '-i', str(recording), '-map', '0:%d' % track['index'], '-ar', '48000', '-f', 's16le', '-'], env=fixture.tool_env)
                assert len(pcm) >= int(7.7 * 48000 * track['channels'] * 2), (track, len(pcm))
                assert any(pcm), 'decoded audio is empty'
        if video:
            poster = Path('/tmp/mist_thumbs') / fixture.stream / 'poster.jpg'
            until(lambda: poster.exists(), 10, 'live source did not produce a thumbnail')
            verify_corner(ffmpeg, poster, 0, 80, 44, 0)
        success = True
        print('PASS actual live', codec, 'decoder, complete output timeline and decoded pixels/audio', flush=True)
        return 0
    finally:
        if not success:
            for name in ('output.log', 'live-metadata.json'):
                path = fixture.root / name
                if path.exists():
                    print(name, path.read_text(errors='replace')[-12000:], file=sys.stderr)
        fixture.close(failed=not success)
        shutil.rmtree(Path('/tmp/mist_thumbs') / fixture.stream, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
