#!/usr/bin/env python3
"""窗口入口的自动冒烟：dummy SDL、真实 UDP、录制/播放及正常释放。"""
import argparse
import os
import signal
import socket
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, default=Path('build/v6-sanitize'))
parser.add_argument('--output', type=Path, default=Path('build/sdl-smoke'))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
environment = os.environ | {'SDL_VIDEODRIVER': 'dummy', 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'}
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
logs = []
processes = []
def launch(name, command):
    log = (args.output / f'{name}.log').open('w')
    logs.append(log)
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=environment)
    processes.append(process)
    return process
try:
    server = launch('server', [str(args.build / 'lab_server'), '--port', str(port), '--record-dir', str(args.output / 'server-replays')])
    time.sleep(.15)
    if server.poll() is not None:
        raise RuntimeError('server initialization failed')
    clients = [launch(f'client{number}', [str(args.build / 'lab_client'), '--server', f'127.0.0.1:{port}', '--frames', '1500'] +
                      (['--record', str(args.output / 'client.jsonl')] if number == 1 else [])) for number in (1, 2)]
    for client in clients:
        if client.wait(timeout=30):
            raise RuntimeError('SDL live client failed')
    server.send_signal(signal.SIGTERM)
    if server.wait(timeout=5):
        raise RuntimeError('server shutdown failed')
    replays = list((args.output / 'server-replays').glob('*.jsonl')) + [args.output / 'client.jsonl']
    if len(replays) < 2:
        raise RuntimeError('missing server/client recordings')
    for replay in replays:
        if subprocess.run([str(args.build / 'lab_replay'), str(replay)], env=environment, check=False).returncode:
            raise RuntimeError('recorded replay verification failed')
    replay = launch('replay-window', [str(args.build / 'lab_client'), '--replay', str(replays[0]), '--frames', '100'])
    if replay.wait(timeout=10):
        raise RuntimeError('SDL replay mode failed')
    print(f'SDL/UDP recording and replay smoke passed; logs: {args.output}')
finally:
    for process in processes:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    for log in logs:
        log.close()
