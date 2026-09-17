#!/usr/bin/env python3
"""Poisson timing diagnostic, with every sampled nBits checked against C++.

This model uses one timestamped template per block, constant hash power, and no
network delay or validation cost. It cannot qualify production propagation,
mobile catch-up, timestamp attacks, or activation readiness.
"""
import importlib.util
import json
from pathlib import Path
import random
import statistics
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "asert_oracle", root / "tests/consensus/test_sixty_second_asert_oracle.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
rows, expected, reports = [], [], []
for hash_factor in (1, 16, 64, 256):
    rng = random.Random(6017)
    activation, anchor_bits, limit_bits = 20001, 0x1d31ffce, 0x1d31ffce
    # Warm up for 10,000 blocks under the deployed legacy rule. Targets above
    # the nominal limit are historical consensus outputs, accepted by the real
    # PoW gate (hash <= target); do not invent a pre-activation rejection rule.
    clock = 1_000_000 + 10000 * 120 - 600000
    intervals, boundary, previous = [], None, None
    for height in range(10001, 30001):
        if height == activation:
            boundary = (height - 1, previous[0], previous[1])
        anchor_height, anchor_time, bits = boundary or (0, 1_000_000, anchor_bits)
        row = (activation, height, anchor_height, anchor_time, bits, limit_bits,
               int(clock), 120, 43200)
        required = m.oracle(row)
        target = m.decode(required)
        assert target > 0
        if height >= activation:
            assert target <= m.decode(limit_bits)
        if height == activation:
            work_ratio = m.decode(previous[1]) / target
        mean = 120 * m.decode(anchor_bits) / (hash_factor * target)
        previous = (int(clock), required)
        elapsed = rng.expovariate(1 / mean)
        clock += elapsed
        intervals.append(elapsed)
        rows.append(row)
        expected.append(required)
    late_mean = statistics.mean(intervals[-5000:])
    expected_mean = max(60, 120 / hash_factor)
    assert abs(late_mean / expected_mean - 1) < 0.05, (hash_factor, late_mean)
    reports.append({
        "hash_factor": hash_factor,
        "expected_mean_at_limit_seconds": 120 / hash_factor,
        "pre_mean_seconds": statistics.mean(intervals[5000:10000]),
        "first_100_new_blocks_mean_seconds": statistics.mean(intervals[10000:10100]),
        "late_mean_seconds": late_mean,
        "boundary_work_ratio": work_ratio,
        "maximum_first_100_gap_seconds": max(intervals[10000:10100]),
    })
payload = ''.join(' '.join(map(str, row)) + '\n' for row in rows)
run = subprocess.run([sys.argv[1]], input=payload, text=True, capture_output=True, check=True)
assert list(map(int, run.stdout.split())) == expected
print(json.dumps({
    "model": "single-template Poisson arrivals; constant hash power; no propagation or validation load",
    "seed": 6017,
    "rows_checked_against_production": len(rows),
    "production_qualified": False,
    "scenarios": reports,
}, indent=2))
