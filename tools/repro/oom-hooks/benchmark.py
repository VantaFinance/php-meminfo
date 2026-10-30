#!/usr/bin/env python3
"""Measure hook overhead using binaries produced by run.py (not Docker startup)."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    args = parser.parse_args()
    output = args.results.resolve()
    provenance = output / "results.json"
    if not provenance.exists():
        provenance = output / "semantics.json"
    image = json.loads(provenance.read_text())["image"]
    (output / "benchmark.php").write_text("""<?php
$stream = fopen('/dev/null', 'w');
if (getenv('PROBE_ARM') === '1') { oom_probe_arm($stream, 32 * 1024 * 1024); }
$sum = 0;
$start = hrtime(true);
for ($i = 0; $i < 1000000; $i++) { $value = str_repeat('x', 128); $sum += strlen($value); }
$elapsed = hrtime(true) - $start;
oom_probe_disarm();
echo json_encode(array('ns' => $elapsed, 'sum' => $sum, 'stats' => oom_probe_stats()));
""")
    variants = [("baseline", "interrupt", "0"), ("allocator", "interrupt", "1"),
                ("opcodes", "opcode", "1"), ("internal", "internal", "1")]
    samples = {name: [] for name, _, _ in variants}
    for round_number in range(6):
        # Interleave and rotate; discard the first round as warm-up.
        for index in range(len(variants)):
            name, mode, armed = variants[(index + round_number) % len(variants)]
            command = ["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}",
                       "-v", f"{output}:/results", "-e", f"MEMINFO_ALLOC_PROBE_MODE={mode}",
                       "-e", f"PROBE_ARM={armed}", image, "php", "-n",
                       "-d", "extension=/src/extension/modules/meminfo.so",
                       "-d", "extension=/results/oom_probe.so", "-d", "memory_limit=16M",
                       "/results/benchmark.php"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60, check=True)
            data = json.loads(result.stdout)
            if data["sum"] != 128000000 or data["stats"]["custom_heap"]:
                raise RuntimeError(f"Benchmark changed workload/allocator state: {data}")
            if round_number:
                samples[name].append(data["ns"] / 1000000)
    baseline = statistics.median(samples["baseline"])
    report = {name: {"samples_ms": values, "median_ms": statistics.median(values),
                     "relative_to_baseline": statistics.median(values) / baseline}
              for name, values in samples.items()}
    (output / "benchmark.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
