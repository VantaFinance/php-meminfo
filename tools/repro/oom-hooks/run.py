#!/usr/bin/env python3
"""Run standalone OOM-hook prototypes in an existing Linux PHP image.

The image must provide cc, php-config and meminfo at
/src/extension/modules/meminfo.so. Results stay outside the source tree.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


def run(command, **kwargs):
    container = None
    if command[:2] == ["docker", "run"]:
        container = "meminfo-oom-probe-" + uuid.uuid4().hex
        command = command[:2] + ["--name", container] + command[2:]
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=90, **kwargs)
    except subprocess.TimeoutExpired:
        if container:
            subprocess.run(["docker", "rm", "-f", container], capture_output=True, timeout=15)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--expected-oom-exit", type=int, default=1)
    parser.add_argument("--semantics", action="store_true",
                        help="run the native allocator equivalence gate instead of capture fixtures")
    parser.add_argument("--backend", choices=("allocator", "internal"), default="allocator")
    parser.add_argument("--jit", action="store_true", help="require OPcache JIT for capture fixtures")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    output = (args.output or Path(tempfile.mkdtemp(prefix="meminfo-oom-hooks-"))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("--output must be empty; stale append-only logs would invalidate the results")
    common = [
        "docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}",
        "-v", f"{source}:/probe:ro", "-v", f"{output}:/results",
    ]
    build = run(common + [args.image, "sh", "-c", """
set -eu
cc -shared -fPIC -g -O1 -Wall -Wextra -Wno-unused-parameter -Werror \
    $(php-config --includes) /probe/allocator_probe.c -o /results/oom_probe.so
cc -shared -fPIC -std=c11 -Wall -Wextra -Werror /probe/exit_probe.c -o /results/exit_probe.so
cc -std=c11 -Wall -Wextra -Werror /probe/exit_control.c -o /results/exit_control
php -n -v
"""])
    (output / "build.log").write_text(build.stdout + build.stderr)
    if build.returncode:
        raise SystemExit(build.stdout + build.stderr)

    if args.semantics:
        if args.jit:
            parser.error("--jit applies to capture fixtures, not the allocator equivalence gate")
        sys.dont_write_bytecode = True
        from semantics import run_semantics
        return run_semantics(common, args.image, output, run, args.backend)

    rows = []
    for status in [0, 1, 37, 255, 257, -1]:
        path = output / f"exit-{status}.log"
        result = run(common + ["-e", "LD_PRELOAD=/results/exit_probe.so", "-e",
                     f"MEMINFO_EXIT_PROBE_LOG=/results/{path.name}", args.image,
                     "/results/exit_control", str(status)])
        text = path.read_text() if path.exists() else ""
        passed = result.returncode == (status & 255) and f"status={status}\n" in text
        rows.append({"case": f"exit-{status}", "passed": passed, "exit": result.returncode})

    # All PHP cases register a shutdown marker, so preserved patched/unpatched
    # OOM behavior is checked independently of the existence of a dump.
    prefix = """<?php
class OomProbeMarker { public $value = 'kept-before-allocation'; }
$marker = new OomProbeMarker();
register_shutdown_function(function () { fwrite(STDERR, "PHP_SHUTDOWN_RAN\\n"); });
$stream = fopen(getenv('PROBE_DUMP_PATH'), 'w+');
if (getenv('PROBE_ARM') === '1') { oom_probe_arm($stream, (int) getenv('PROBE_THRESHOLD')); }
"""
    if args.jit:
        prefix += """
$jit = opcache_get_status(false);
if (empty($jit['jit']['on'])) { fwrite(STDERR, "error: JIT is not active\\n"); exit(88); }
"""
    gradual = "$grow = array(); while (true) { $grow[] = str_repeat('x', 1024); }\n"
    straightline = "$grow = array();\n" + "$grow[] = str_repeat('x', 32768);\n" * 700
    large = "$grow = str_repeat('x', 64 * 1024 * 1024);\n"
    realloc = "$grow = array(); for ($i=0; $i<4000000; $i++) { $grow[]=$i; }\n"
    normal = """
$grow = array(); for ($i=0; $i<4500; $i++) { $grow[] = str_repeat('x', 1024); }
oom_probe_disarm();
echo 'STATS=', json_encode(oom_probe_stats()), "\\n";
"""
    cases = [
        # name, mode, body, armed, expect dump, expect notify, threshold, normal exit
        ("baseline-gradual", "interrupt", gradual, False, False, False, 12, False),
        ("notify-gradual", "notify", gradual, True, False, True, 12, False),
        ("interrupt-gradual", "interrupt", gradual, True, True, True, 12, False),
        ("opcode-gradual", "opcode", gradual, True, True, True, 12, False),
        ("interrupt-straightline", "interrupt", straightline, True, None, True, 12, False),
        ("opcode-straightline", "opcode", straightline, True, True, True, 12, False),
        ("interrupt-large", "interrupt", large, True, False, True, 12, False),
        ("opcode-large", "opcode", large, True, False, True, 12, False),
        ("interrupt-realloc", "interrupt", realloc, True, True, True, 12, False),
        ("normal-cleanup", "interrupt", normal, True, False, False, 12, True),
        ("normal-after-dump", "interrupt", normal, True, True, True, 4, True),
    ]
    if args.backend == "internal":
        cases = [case for case in cases if not case[0].startswith("opcode-")]
        cases = [(name, "internal-notify" if mode == "notify" else "internal", body,
                  armed, False if name == "interrupt-realloc" else dump,
                  False if name in ("interrupt-realloc", "interrupt-large") else notify,
                  threshold, normal_exit)
                 for name, mode, body, armed, dump, notify, threshold, normal_exit in cases]
    for name, mode, body, armed, expect_dump, expect_notify, threshold, normal_exit in cases:
        script = output / f"{name}.php"
        script.write_text(prefix + body)
        environment = {
            "MEMINFO_ALLOC_PROBE_MODE": mode,
            "MEMINFO_ALLOC_PROBE_LOG": f"/results/{name}.allocator.log",
            "MEMINFO_EXIT_PROBE_LOG": f"/results/{name}.exit.log",
            "LD_PRELOAD": "/results/exit_probe.so",
            "PROBE_DUMP_PATH": f"/results/{name}.dump.json",
            "PROBE_THRESHOLD": str(threshold * 1024 * 1024),
            "PROBE_ARM": "1" if armed else "0",
        }
        command = common[:]
        for key, value in environment.items():
            command += ["-e", f"{key}={value}"]
        command += [args.image, "php", "-n", "-d", "extension=/src/extension/modules/meminfo.so",
                    "-d", "extension=/results/oom_probe.so", "-d", "memory_limit=16M",
                    ]
        if args.jit:
            command += ["-d", "zend_extension=opcache", "-d", "opcache.enable_cli=1",
                        "-d", "opcache.jit_buffer_size=32M", "-d", "opcache.jit=tracing",
                        "-d", "opcache.jit_hot_loop=1", "-d", "opcache.jit_hot_func=1",
                        "-d", "opcache.file_update_protection=0"]
        command += [f"/results/{script.name}"]
        started = time.monotonic()
        result = run(command)
        seconds = round(time.monotonic() - started, 3)
        (output / f"{name}.stdout").write_text(result.stdout)
        (output / f"{name}.stderr").write_text(result.stderr)
        allocator_path = output / f"{name}.allocator.log"
        allocator = allocator_path.read_text() if allocator_path.exists() else ""
        exit_path = output / f"{name}.exit.log"
        exit_log = exit_path.read_text() if exit_path.exists() else ""
        dump_path = output / f"{name}.dump.json"
        valid = False
        marker = False
        error = None
        if dump_path.exists() and dump_path.stat().st_size:
            try:
                data = json.loads(dump_path.read_text())
                valid = isinstance(data.get("header"), dict) and isinstance(data.get("items"), dict)
                marker = any(item.get("class") == "OomProbeMarker" for item in data.get("items", {}).values())
            except (ValueError, AttributeError) as exc:
                error = str(exc)
        notified = "phase=notify " in allocator
        shutdown = "PHP_SHUTDOWN_RAN" in result.stderr
        expected_exit = 0 if normal_exit else args.expected_oom_exit
        expected_shutdown = normal_exit or args.expected_oom_exit == 255
        passed = result.returncode == expected_exit and shutdown == expected_shutdown
        passed &= notified == expect_notify
        if expect_dump is not None:
            passed &= valid == expect_dump
        if valid:
            passed &= marker and "phase=dump-ok " in allocator
            passed &= any("phase=dump-ok " in line and "shutdown=0" in line
                          for line in allocator.splitlines())
        if not normal_exit and args.expected_oom_exit == 1:
            passed &= "status=1\n" in exit_log
        if normal_exit:
            stats_line = next((line[6:] for line in result.stdout.splitlines() if line.startswith("STATS=")), "{}")
            stats = json.loads(stats_line)
            passed &= stats.get("active") is False and stats.get("custom_heap") is False
        row = {"case": name, "passed": bool(passed), "exit": result.returncode,
               "notified": notified, "valid_dump": valid, "marker": marker,
               "shutdown": shutdown, "dump_bytes": dump_path.stat().st_size if dump_path.exists() else 0,
               "seconds": seconds, "parse_error": error}
        rows.append(row)
        print(json.dumps(row), flush=True)

    report = {"image": args.image, "backend": args.backend, "jit": args.jit, "expected_oom_exit": args.expected_oom_exit,
              "results": rows, "all_passed": all(row["passed"] for row in rows)}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Results: {output}", flush=True)
    return 0 if report["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
