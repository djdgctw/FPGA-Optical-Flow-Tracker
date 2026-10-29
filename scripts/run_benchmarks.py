#!/usr/bin/env python3
"""Run the formal KV260 image with serial and overlapped prepared-feature inputs."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('images', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--pairs', type=int, default=1000)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--points-prefix', type=Path)
    args = parser.parse_args()
    if args.pairs < 1 or args.rounds < 1:
        parser.error('--pairs and --rounds must be positive')
    images = args.images.resolve()
    if not images.is_dir() or len(list(images.glob('*.png'))) < args.pairs + 1:
        parser.error('IMAGE_DIR must contain at least PAIRS+1 PNG files')
    host = root / 'build/benchmark_kv260_lk'
    xclbin = root / 'hardware/lk_coeff_triple.xclbin'
    if not host.is_file():
        parser.error('build the host first with ./host/build.sh')
    reference = args.points_prefix.resolve() if args.points_prefix else None
    if reference:
        for ext in ('.counts.bin', '.points.bin', '.files.txt'):
            if not Path(str(reference) + ext).is_file():
                parser.error('missing points-prefix file: ' + str(reference) + ext)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = {k: v for k, v in os.environ.items() if not k.startswith('LK_')}
    environment.update(LK_HALFPIX_NEON='1', LK_BENCH_KERNEL='lk_coeff_triple')
    metadata = dict(started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                    pairs=args.pairs, rounds=args.rounds,
                    host_sha256=digest(host), xclbin_sha256=digest(xclbin),
                    cpu_online=Path('/sys/devices/system/cpu/online').read_text().strip(),
                    inputs='decoded images and prepared feature coordinates', runs=[])
    reference_outputs = None
    expected_inputs = None
    extensions = ('.counts.bin', '.points.bin', '.files.txt')
    for repeat in range(args.rounds):
        modes = [('prepared-serial', 1, 2), ('prepared-pipeline', 3, 8)]
        if repeat % 2:
            modes.reverse()
        for mode, workers, slots in modes:
            prefix = output / f'r{repeat}_{mode}'
            env = dict(environment)
            if reference:
                env['LK_POINTS_PREFIX'] = str(reference)
            command = [str(host), str(xclbin), str(images), str(args.pairs), mode,
                       str(workers), str(slots), 'direct', 'reuse', str(prefix)]
            print(f'RUN round={repeat} mode={mode}', flush=True)
            completed = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, timeout=1800)
            Path(str(prefix) + '.log').write_text(completed.stdout)
            print(completed.stdout.strip(), flush=True)
            completed.check_returncode()
            record = json.loads(Path(str(prefix) + '.json').read_text())
            if record['pairs'] != args.pairs or record['physical_lanes'] != 3:
                raise RuntimeError('unexpected workload or lane count')
            inputs = {ext: digest(Path(str(prefix) + ext)) for ext in extensions}
            if expected_inputs is None:
                expected_inputs = {ext: digest(Path(str(reference) + ext)) for ext in extensions} if reference else inputs
            if inputs != expected_inputs:
                raise RuntimeError('input feature counts, coordinates, or image order differ')
            outputs = {ext: digest(Path(str(prefix) + ext)) for ext in ('.flows.bin', '.coords.bin')}
            if reference_outputs is None:
                reference_outputs = outputs
            if outputs != reference_outputs:
                raise RuntimeError('raw flow or recovered coordinates differ between configurations')
            if reference is None:
                reference = prefix
            counts_data = Path(str(prefix) + '.counts.bin').read_bytes()
            if len(counts_data) != args.pairs * 4:
                raise RuntimeError('invalid feature count file size')
            counts = [v[0] for v in struct.iter_unpack('<i', counts_data)]
            if any(n < 0 or n > 150 for n in counts):
                raise RuntimeError('invalid feature count')
            task_rows = []
            for frame, n in enumerate(counts):
                per_lane = (n + 2) // 3
                row = dict(frame=frame, valid_features=n, executed_tasks=3*per_lane,
                           padding_tasks=3*per_lane-n)
                for lane in range(3):
                    valid = max(0, (n + 2 - lane) // 3)
                    row.update({f'lane{lane}_valid': valid,
                                f'lane{lane}_executed': per_lane,
                                f'lane{lane}_padding': per_lane-valid})
                task_rows.append(row)
            with Path(str(prefix) + '.tasks.csv').open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(task_rows[0]))
                writer.writeheader()
                writer.writerows(task_rows)
            record.update(round=repeat, prefix=prefix.name, input_sha256=inputs,
                          output_sha256=outputs, outputs_match=True,
                          executed_tasks=sum(r['executed_tasks'] for r in task_rows),
                          padding_tasks=sum(r['padding_tasks'] for r in task_rows),
                          task_counts_source='host grouping rule, not hardware counters')
            metadata['runs'].append(record)
            (output / 'summary.json').write_text(json.dumps(metadata, indent=2) + '\n')
    metadata['summary'] = []
    for mode in ('prepared-serial', 'prepared-pipeline'):
        runs = [r for r in metadata['runs'] if r['mode'] == mode]
        pairs = sum(r['pairs'] for r in runs)
        wall_ms = sum(r['wall_ms'] for r in runs)
        fields = list(runs[0]['mean_ms'])
        metadata['summary'].append(dict(mode=mode, total_pairs=pairs, total_wall_ms=wall_ms,
            pooled_fps=1000*pairs/wall_ms, interval_ms=wall_ms/pairs,
            mean_features=sum(r['features'] for r in runs)/pairs,
            mean_ms={field: sum(r['pairs']*r['mean_ms'][field] for r in runs)/pairs for field in fields}))
    metadata['completed_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    (output / 'summary.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata['summary'], indent=2))


if __name__ == '__main__':
    main()
