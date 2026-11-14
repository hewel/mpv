#!/usr/bin/env python3
"""Headless JSON IPC contract regression for dual subtitles and A/B loops.

Run after building mpv and the existing locale.mkv/multilang.mkv test samples:
    python3 test/dual_subtitle_ab.py --mpv build/mpv --samples build/test/samples

Uses a POSIX socket pair, null outputs, synthetic samples, and event deadlines.
It checks actual selection/loop behavior, not subtitle appearance or GPU output.
"""

import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def check(condition, detail):
    if not condition:
        raise AssertionError(detail)


class Player:
    def __init__(self, executable):
        self.socket, child = socket.socketpair()
        self.stderr = tempfile.TemporaryFile()
        try:
            self.process = subprocess.Popen([
                str(executable.resolve()), '--no-config', '--idle=yes',
                '--pause=yes', '--keep-open=yes', '--vo=null', '--ao=null',
                '--hwdec=no', '--load-scripts=no', '--osc=no', '--ytdl=no',
                '--terminal=no', '--input-terminal=no', '--sub-auto=no',
                '--resume-playback=no', '--save-position-on-quit=no',
                f'--input-ipc-client=fd://{child.fileno()}',
            ], pass_fds=(child.fileno(),), stdout=subprocess.DEVNULL,
                stderr=self.stderr)
        finally:
            child.close()
        self.buffer = b''
        self.sequence = 0
        self.events = []
        self.transcript = []

    def receive(self, deadline):
        while b'\n' not in self.buffer:
            remaining = deadline - time.monotonic()
            check(remaining > 0, 'Timed out waiting for IPC event/reply')
            self.socket.settimeout(remaining)
            data = self.socket.recv(1048576)
            check(data, 'mpv IPC closed unexpectedly')
            self.buffer += data
        line, self.buffer = self.buffer.split(b'\n', 1)
        response = json.loads(line)
        self.transcript.append({'receive': response})
        if 'event' in response:
            self.events.append(response)
        return response

    def command(self, *args, error='success'):
        self.sequence += 1
        request = {'command': args, 'request_id': self.sequence}
        self.transcript.append({'send': request})
        self.socket.sendall(json.dumps(request).encode() + b'\n')
        deadline = time.monotonic() + 10
        while True:
            response = self.receive(deadline)
            if response.get('request_id') == self.sequence:
                check(response['error'] == error, (request, response, error))
                return response.get('data')

    def get(self, name):
        return self.command('get_property', name)

    def set(self, name, value):
        return self.command('set_property', name, value)

    def wait(self, predicate, start):
        deadline = time.monotonic() + 10
        while True:
            for event in self.events[start:]:
                if predicate(event):
                    return event
            start = len(self.events)
            self.receive(deadline)

    def wait_event(self, name, start):
        return self.wait(lambda e: e['event'] == name, start)

    def load(self, path):
        self.set('pause', True)
        start = len(self.events)
        self.command('loadfile', str(path.resolve()))
        self.wait_event('file-loaded', start)
        self.wait_event('playback-restart', start)

    def seek(self, seconds):
        start = len(self.events)
        self.command('seek', seconds, 'absolute+exact')
        self.wait_event('playback-restart', start)

    def tracks(self):
        return [t for t in self.get('track-list') if t['type'] == 'sub']

    def selected(self, primary, secondary):
        check(self.get('sid') == primary, 'Primary selection changed unexpectedly')
        actual = self.get('secondary-sid')
        check(actual == secondary and type(actual) is type(secondary),
              ('Secondary selection mismatch', actual, secondary))

    def stop(self):
        start = len(self.events)
        self.command('stop')
        self.wait_event('end-file', start)
        self.wait_event('idle', start)

    def close(self):
        try:
            if self.process.poll() is None:
                self.command('quit')
        except (OSError, AssertionError):
            pass
        finally:
            self.socket.close()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            self.stderr.close()


def run(player, samples, scratch):
    p = player
    for observer, name in enumerate([
        'track-list', 'sid', 'secondary-sid', 'secondary-sub-visibility',
        'secondary-sub-pos', 'secondary-sub-text', 'ab-loop-a', 'ab-loop-b',
        'ab-loop-count', 'remaining-ab-loops', 'eof-reached',
    ], 1):
        p.command('observe_property', observer, name)
    p.command('get_property', 'secondary-sub-text', error='property unavailable')
    p.command('get_property', 'secondary-sub-position', error='property not found')
    unavailable = p.wait(lambda e: e.get('name') == 'secondary-sub-text', 0)
    check('data' not in unavailable, 'Unavailable observation must not carry text')

    p.load(samples / 'locale.mkv')
    p.set('sid', 1)
    p.set('secondary-sid', 2)
    p.selected(1, 2)
    check({t['id']: t.get('main-selection') for t in p.tracks()} == {1: 0, 2: 1},
          'Both subtitle roles must be reflected in track-list')
    p.set('secondary-sid', 1)
    p.selected(1, 2)
    p.set('sid', 2)
    p.selected(1, 2)
    # Promotion must explicitly vacate the secondary slot first.
    p.set('secondary-sid', 'no')
    p.set('sid', 2)
    p.set('secondary-sid', 1)
    p.selected(2, 1)
    p.set('secondary-sid', 99)
    p.selected(2, False)
    p.set('sid', 1)
    p.set('secondary-sid', 2)
    p.command('sub-remove', 2)  # Embedded tracks cannot be removed.
    p.selected(1, 2)
    check(len(p.tracks()) == 2, 'Removing an embedded track must not remove it')
    print('PASS dual selection, duplicate rejection, primary promotion, embedded removal')

    external = scratch / 'second.srt'
    external.write_text('1\n00:00:00,000 --> 00:00:02,000\nSECOND\n')
    p.command('sub-add', str(external), 'auto')
    p.selected(1, 2)
    added = next(t['id'] for t in p.tracks()
                 if t.get('external-filename') == str(external))
    p.set('secondary-sid', added)
    p.seek(0.5)
    before = p.get('time-pos')
    p.set('secondary-sub-visibility', False)
    p.set('secondary-sub-pos', 15)
    p.selected(1, added)
    check(p.get('pause') and p.get('time-pos') == before,
          'Subtitle changes must not resume or seek paused playback')
    check(p.get('sub-text') == 'foo' and p.get('secondary-sub-text') == 'SECOND',
          'Hidden secondary text should remain decoded independently of primary')
    start = len(p.events)
    p.command('sub-remove', added)
    p.selected(1, False)
    p.command('get_property', 'secondary-sub-text', error='property unavailable')
    # Text notifications are tick-driven and can remain stale while paused.
    # A removed selection is the reliable signal to clear consumer text state.
    p.wait(lambda e: e.get('name') == 'secondary-sid' and e.get('data') is False, start)
    check(all(t['id'] != added for t in p.tracks()), 'External track still exists')
    p.command('sub-remove', added, error='error running command')
    print('PASS external secondary add, hidden decoding, paused state, deletion events')

    bitmap = Path(__file__).resolve().parent / 'samples/img_sub.idx'
    p.command('sub-add', str(bitmap), 'auto')
    added = next(t['id'] for t in p.tracks()
                 if t.get('external-filename') == str(bitmap))
    p.set('secondary-sid', added)
    p.seek(0.1)
    p.selected(1, added)
    check(p.get('current-tracks/sub2/codec') == 'dvd_subtitle', 'Bitmap not selected')
    check(p.get('current-tracks/sub2/image') is False, 'image is not a bitmap-sub flag')
    check(p.get('secondary-sub-text') == '', 'Bitmap subtitles have no extracted text')
    p.command('sub-remove', added)
    print('PASS bitmap selection capability and text/classification limits (no rendering)')

    p.load(samples / 'locale.mkv')
    p.set('secondary-sid', 'no')
    p.set('sid', 1)
    p.set('secondary-sid', 2)
    p.selected(1, 2)
    p.set('ab-loop-count', 2)
    p.set('ab-loop-a', 0.2)
    p.set('ab-loop-b', 0.4)
    p.load(samples / 'locale.mkv')
    p.selected(1, 2)
    p.stop()
    check(p.get('track-list') == [], 'stop must remove active tracks')
    p.selected(1, 2)  # Idle getters expose options, not active tracks.
    check(p.get('ab-loop-a') == 0.2 and p.get('ab-loop-b') == 0.4,
          'Global A/B markers unexpectedly cleared on stop')
    p.load(samples / 'multilang.mkv')
    check(p.get('secondary-sid') is False, 'Changed layout must invalidate explicit secondary ID')
    check(p.get('ab-loop-a') == 0.2 and p.get('remaining-ab-loops') == 2,
          'New file must retain global markers and reload loop budget')
    check(p.get('secondary-sub-visibility') is False and p.get('secondary-sub-pos') == 15,
          'Global subtitle settings unexpectedly cleared')
    print('PASS reload/stop/layout-change semantics and persistent global options')

    # Keep both markers unchanged: only the count may re-arm clipping.
    p.set('ab-loop-count', 0)
    p.seek(0.3)
    paused_position = p.get('time-pos')
    p.set('ab-loop-count', 2)
    check(p.get('pause') and p.get('time-pos') == paused_position,
          'Count-only enabling must not seek or resume paused playback')
    start = len(p.events)
    p.set('pause', False)
    p.wait(lambda e: (e.get('name') == 'remaining-ab-loops' and e.get('data') == 0) or
           (e.get('name') == 'eof-reached' and e.get('data') is True), start)
    check(p.get('remaining-ab-loops') == 0,
          'Count-only enabling failed to re-arm looping before EOF')
    # Budget notification alone is not evidence of completed playback restart.
    p.wait(lambda _: sum(e['event'] == 'playback-restart'
                        for e in p.events[start:]) >= 2, start)
    p.set('pause', True)
    events = p.events[start:]
    check(sum(e['event'] == 'seek' for e in events) == 2, 'Expected two actual loop seeks')
    check(p.get('ab-loop-count') == 2 and p.get('remaining-ab-loops') == 0,
          'Loop count and remaining budget must be separate')
    p.set('ab-loop-count', 2)
    p.seek(1.1)
    check(p.get('ab-loop-a') == 0.2 and p.get('ab-loop-b') == 0.4 and
          p.get('remaining-ab-loops') == 2 and p.get('time-pos') >= 1.1,
          'Manual seek past B must retain markers/budget without immediate pullback')
    print('PASS count-only A/B re-enable, two loop seek/restart pairs, manual seek past B')

    p.set('ab-loop-a', 'no')
    p.set('ab-loop-b', 'no')
    p.set('ab-loop-count', 'inf')
    primary = p.get('sid')
    secondary = next(t['id'] for t in p.tracks() if t['id'] != primary)
    p.set('file-local-options/ab-loop-count', 3)
    p.set('file-local-options/ab-loop-a', 0.2)
    p.set('file-local-options/ab-loop-b', 0.4)
    p.set('file-local-options/secondary-sid', secondary)
    p.selected(primary, secondary)
    check(p.get('current-tracks/sub2/id') == secondary and
          p.get('ab-loop-a') == 0.2 and p.get('ab-loop-b') == 0.4 and
          p.get('ab-loop-count') == 3 and p.get('remaining-ab-loops') == 3,
          'File-local writes must take effect before testing their restoration')
    p.stop()
    check(p.get('ab-loop-a') == 'no' and p.get('ab-loop-b') == 'no' and
          p.get('ab-loop-count') == 'inf' and
          p.get('secondary-sid') is False,
          'File-local marker/selection writes must restore previous options at unload')
    print('PASS file-local cleanup at stop')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mpv', type=Path, default=Path('build/mpv'))
    parser.add_argument('--samples', type=Path, default=Path('build/test/samples'))
    parser.add_argument('--transcript', type=Path)
    args = parser.parse_args()
    for name in ('locale.mkv', 'multilang.mkv'):
        check((args.samples / name).is_file(), f'Build existing sample first: {name}')
    player = Player(args.mpv)
    try:
        with tempfile.TemporaryDirectory(prefix='mpv-dual-subtitle-ab-') as directory:
            run(player, args.samples, Path(directory))
    finally:
        player.close()
        if args.transcript:
            args.transcript.write_text(json.dumps(player.transcript, indent=2) + '\n')


if __name__ == '__main__':
    main()
