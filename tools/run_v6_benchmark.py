#!/usr/bin/env python3
"""v6 同版本对照：串行运行固定场景，保留原始 JSON 与二进制摘要。"""
import argparse
import hashlib
import json
import platform
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--warmup', type=float, default=5)
parser.add_argument('--sample', type=float, default=30)
parser.add_argument('--repeat', type=int, default=3)
parser.add_argument('--modes', nargs='+', choices=['core', 'udp'], default=['core', 'udp'])
parser.add_argument('--rooms', nargs='+', type=int, default=[1, 16, 64])
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
manifest = {'binary': str(args.binary.resolve()), 'sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            'machine': platform.uname()._asdict(), 'commands': [], 'reports': []}
try:
    manifest['hardware'] = subprocess.check_output(['sysctl', 'hw.model', 'hw.memsize', 'hw.ncpu', 'machdep.cpu.brand_string'], text=True).strip()
except (OSError, subprocess.CalledProcessError):
    manifest['hardware'] = 'unavailable'
for mode in args.modes:
    for rooms in args.rooms:
        output = args.output / f'{mode}_{rooms}.json'
        command = [str(args.binary.resolve()), '--mode', mode, '--rooms', str(rooms), '--warmup', str(args.warmup),
                   '--sample', str(args.sample), '--repeat', str(args.repeat), '--json', str(output.resolve())]
        print(f'Benchmark: {mode}, {rooms} rooms', flush=True)
        manifest['commands'].append(command)
        with (args.output / f'{mode}_{rooms}.stdout.json').open('w') as stdout:
            result = subprocess.run(command, stdout=stdout, check=False)
        if result.returncode:
            manifest['failure'] = {'command': command, 'exit': result.returncode}
            (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
            raise SystemExit(result.returncode)
        manifest['reports'].append(json.loads(output.read_text()))
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
print(f'Reports: {args.output}', flush=True)
