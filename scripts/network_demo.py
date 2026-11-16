#!/usr/bin/env python3
"""Run the real gateway, observer and order clients on an ephemeral loopback port."""
import argparse
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import tempfile
import time


class Stream:
    def __init__(self, process):
        self.process = process
        self.data = bytearray()

    def until(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            while not predicate(self.data.decode(errors='replace')):
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not selector.select(remaining):
                    raise RuntimeError('Timed out waiting for demo output: ' + self.data.decode(errors='replace'))
                chunk = os.read(self.process.stdout.fileno(), 65536)
                if not chunk:
                    raise RuntimeError('Demo process disconnected: ' + self.data.decode(errors='replace'))
                self.data.extend(chunk)
        return self.data.decode(errors='replace')


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path('build/debug'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    binaries = args.build_dir.resolve()
    (root / 'tmp').mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='network-demo-', dir=root / 'tmp'))
    journal = output / 'session.apex'
    server = observer = maker = None
    try:
        server = subprocess.Popen([str(binaries / 'apex_gateway'), '--port', '0', '--journal', str(journal)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        server_stream = Stream(server)
        startup = server_stream.until(lambda text: re.search(r'listening on 127\.0\.0\.1:(\d+)', text))
        port = re.search(r'listening on 127\.0\.0\.1:(\d+)', startup).group(1)
        observer = subprocess.Popen([str(binaries / 'apex_client'), '--port', port, '--participant', '0', '--watch'],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        feed = Stream(observer)
        feed.until(lambda text: 'BOOK feed=0 command=0' in text)
        maker = subprocess.Popen([str(binaries / 'apex_client'), '--port', port, '--participant', '1'],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        maker_stream = Stream(maker)
        maker.stdin.write(b'BUY 1 100 10000\n'); maker.stdin.flush()
        maker_stream.until(lambda value: 'UPDATE order=1 remaining=100' in value)
        clients = []
        for participant, command in [(2, 'BUY 2 50 10000'),
                                     (3, 'SELL 3 120 9900'), (2, 'REPLACE 2 4 40 10100'),
                                     (4, 'SELL 5 60 10200'), (5, 'MARKET_BUY 6 80'), (2, 'CANCEL 4')]:
            result = subprocess.run([str(binaries / 'apex_client'), '--port', port, '--participant', str(participant),
                                     '--command', command], capture_output=True, text=True, timeout=5)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            clients.append(command + '\n' + result.stdout)
        # Maker execution reports must display while stdin remains open/idle.
        maker_stream.until(lambda value: 'UPDATE order=1 remaining=0' in value)
        maker.stdin.write(b'QUIT\n'); maker.stdin.flush()
        remaining, _ = maker.communicate(timeout=5)
        maker_stream.data.extend(remaining)
        if maker.returncode:
            raise RuntimeError(maker_stream.data.decode(errors='replace'))
        clients.insert(0, 'Interactive maker: BUY 1 100 10000\n' + maker_stream.data.decode(errors='replace'))
        text = feed.until(lambda value: 'command=7' in value)
        for expected in ['quantity=100 price_ticks=10000', 'quantity=20 price_ticks=10000', 'quantity=60 price_ticks=10200']:
            if expected not in text:
                raise RuntimeError('Missing expected network trade: ' + expected)
        if 'FEED GAP' in text:
            raise RuntimeError('Unexpected feed gap')
        server.send_signal(signal.SIGINT)
        remaining, _ = server.communicate(timeout=5)
        server_stream.data.extend(remaining)
        if server.returncode:
            raise RuntimeError(server_stream.data.decode(errors='replace'))
        remaining, _ = observer.communicate(timeout=5)
        feed.data.extend(remaining)
        replay = subprocess.run([str(binaries / 'apex'), '--replay', str(journal)],
                                capture_output=True, text=True, timeout=5)
        if replay.returncode or 'commands=7 events=22' not in replay.stdout:
            raise RuntimeError(replay.stdout + replay.stderr)
        (output / 'server.txt').write_bytes(server_stream.data)
        (output / 'feed.txt').write_bytes(feed.data)
        (output / 'clients.txt').write_text('\n'.join(clients))
        (output / 'replay.txt').write_text(replay.stdout)
        print('\n'.join(clients))
        print('Observed live feed:\n' + text)
        print(replay.stdout)
        print('Artifacts:', output)
        print('Network demo verified: 7 commands, 3 trades, continuous feed and journal replay.')
    finally:
        stop(maker)
        stop(observer)
        stop(server)


if __name__ == '__main__':
    main()
