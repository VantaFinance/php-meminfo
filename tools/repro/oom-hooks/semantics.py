"""Differential feasibility gate, run via run.py --semantics.

An exit status of 1 means the experimental backend changed native behavior.
This deliberately reports counterexamples rather than accepting them as passes.
"""
import json


PHP_OPTIONS = ["php", "-n", "-d", "extension=/src/extension/modules/meminfo.so",
               "-d", "extension=/results/oom_probe.so"]


def run_semantics(common, image, output, execute, backend="allocator"):
    rows = []
    mode = "internal" if backend == "internal" else "notify"
    prefix = """<?php
$stream = fopen('/dev/null', 'w');
if (getenv('PROBE_ARM') === '1') { oom_probe_arm($stream, 64 * 1024 * 1024); }
"""
    release = """
$a = array();
for ($i = 0; $i < 15000; $i++) { $a[] = str_repeat('x', 512); }
unset($a);
"""
    # Every pair starts in fresh processes, with identical code and limits.
    cases = [
        ("explicit-gc", "64M", release + """
$before = memory_get_usage(true);
$reclaimed = gc_mem_caches();
echo 'RESULT=', json_encode(array('before' => $before, 'reclaimed' => $reclaimed,
    'after' => memory_get_usage(true))), "\\n";
"""),
        ("automatic-gc-at-limit", "16M", release + """
$large = str_repeat('y', 9 * 1024 * 1024);
echo 'RESULT=', json_encode(array('completed' => strlen($large))), "\\n";
"""),
        ("allocate-free-churn", "32M", """
$sum = 0;
for ($round = 0; $round < 20; $round++) {
    $a = array();
    for ($i = 0; $i < 6000; $i++) { $a[] = str_repeat('x', 512); }
    $sum += count($a); unset($a);
}
echo 'RESULT=', json_encode(array('sum' => $sum)), "\\n";
"""),
        ("changed-limit-enforced", "64M", """
$old = ini_set('memory_limit', '16M');
echo 'RESULT=', json_encode(array('old' => $old, 'configured' => ini_get('memory_limit'))), "\\n";
$large = str_repeat('x', 32 * 1024 * 1024);
echo "UNEXPECTED_ALLOCATION_COMPLETED\\n";
"""),
        ("unlimited-to-finite", "-1", """
$old = ini_set('memory_limit', '16M');
echo 'RESULT=', json_encode(array('old' => $old, 'configured' => ini_get('memory_limit'))), "\\n";
$large = str_repeat('x', 32 * 1024 * 1024);
echo "UNEXPECTED_ALLOCATION_COMPLETED\\n";
"""),
        ("explicit-disarm-gc", "64M", release + """
oom_probe_disarm();
$reclaimed = gc_mem_caches();
echo 'RESULT=', json_encode(array('reclaimed' => $reclaimed,
    'after' => memory_get_usage(true), 'custom_heap' => oom_probe_stats()['custom_heap'])), "\\n";
"""),
    ]

    def php_case(name, script, armed, limit="64M", extra_env=None):
        (output / (name + ".php")).write_text(script)
        command = common + ["-e", f"MEMINFO_ALLOC_PROBE_MODE={mode}", "-e",
                            f"PROBE_ARM={int(armed)}", "-e",
                            f"MEMINFO_ALLOC_PROBE_LOG=/results/{name}.allocator.log"]
        for key, value in (extra_env or {}).items():
            command += ["-e", f"{key}={value}"]
        result = execute(command + [image] + PHP_OPTIONS + ["-d", f"memory_limit={limit}",
                                                          f"/results/{name}.php"])
        (output / (name + ".stdout")).write_text(result.stdout)
        (output / (name + ".stderr")).write_text(result.stderr)
        payloads = [json.loads(line[7:]) for line in result.stdout.splitlines()
                    if line.startswith("RESULT=")]
        return {"exit": result.returncode, "data": payloads,
                "unexpected_completion": "UNEXPECTED_ALLOCATION_COMPLETED" in result.stdout}

    def record(name, passed, **evidence):
        row = {"case": name, "passed": bool(passed), **evidence}
        rows.append(row)
        print(json.dumps(row), flush=True)

    for name, limit, body in cases:
        baseline = php_case(name + "-baseline", prefix + body, False, limit)
        monitored = php_case(name + "-armed", prefix + body, True, limit)
        equal = baseline == monitored
        # A broken control cannot establish equivalence merely by matching.
        if name in ("changed-limit-enforced", "unlimited-to-finite"):
            control_valid = baseline["exit"] in (1, 255) and not baseline["unexpected_completion"]
            control_valid &= bool(baseline["data"]) and baseline["data"][0]["configured"] == "16M"
        else:
            control_valid = baseline["exit"] == 0 and bool(baseline["data"])
        if name in ("explicit-gc", "explicit-disarm-gc") and control_valid:
            control_valid &= baseline["data"][0]["reclaimed"] > 0
            # Allocation bookkeeping can shift a few pages. Test reclamation
            # semantics, not byte-identical allocator placement in two processes.
            equal = monitored["exit"] == 0 and bool(monitored["data"])
            if equal:
                native = baseline["data"][0]
                custom = monitored["data"][0]
                equal &= custom["reclaimed"] >= native["reclaimed"] * 0.90
                equal &= custom["after"] <= native["after"] + 2 * 1024 * 1024
                if name == "explicit-disarm-gc":
                    equal &= custom["custom_heap"] is False
        record(name, equal and control_valid, control_valid=bool(control_valid),
               baseline=baseline, monitored=monitored)

    rearm = php_case("rearm", """<?php
$stream = fopen('/dev/null', 'w');
$ok = true;
for ($i = 0; $i < 3; $i++) {
    oom_probe_arm($stream, 64 * 1024 * 1024);
    $stats = oom_probe_stats();
    $ok = $ok && $stats['active'] && $stats['owns_handlers'];
    oom_probe_disarm();
    $stats = oom_probe_stats();
    $ok = $ok && !$stats['active'] && !$stats['custom_heap'];
}
echo 'RESULT=', json_encode(array('ok' => $ok)), "\\n";
""", False)
    record("handler-ownership-and-rearm", rearm["exit"] == 0
           and rearm["data"] == [{"ok": True}], observed=rearm)

    refusal = php_case("existing-custom-heap", """<?php
$stream = fopen('/dev/null', 'w');
$rejected = false;
try { oom_probe_arm($stream, 64 * 1024 * 1024); }
catch (Exception $exception) { $rejected = true; }
$stats = oom_probe_stats();
echo 'RESULT=', json_encode(array('rejected' => $rejected,
    'custom_heap' => $stats['custom_heap'], 'active' => $stats['active'])), "\\n";
""", False, extra_env={"USE_ZEND_ALLOC": "0"})
    record("reject-existing-custom-heap", refusal["exit"] == 0 and refusal["data"] == [
        {"rejected": True, "custom_heap": True, "active": False}], observed=refusal)

    automatic = php_case("automatic-cleanup", prefix + """
echo 'RESULT=', json_encode(array('owned' => oom_probe_stats()['owns_handlers'])), "\\n";
""", True)
    cleanup_log = (output / "automatic-cleanup.allocator.log").read_text()
    record("automatic-rshutdown", automatic["exit"] == 0
           and automatic["data"] == [{"owned": True}]
           and "phase=disarm " in cleanup_log and "phase=rshutdown-finished " in cleanup_log,
           observed=automatic)

    # Real repeated requests in one CLI-server process: no explicit disarm.
    (output / "router.php").write_text("""<?php
$before = oom_probe_stats();
if (isset($_GET['arm'])) {
    if (getenv('ARMED_SEQUENCE') === '1') {
        $stream = fopen('/dev/null', 'w');
        oom_probe_arm($stream, 64 * 1024 * 1024);
    }
    $a = array();
    for ($i = 0; $i < 15000; $i++) { $a[] = str_repeat('x', 512); }
}
echo json_encode(array('pid' => getmypid(), 'before' => $before,
    'after' => oom_probe_stats(), 'real' => memory_get_usage(true)));
""")
    (output / "client.php").write_text("""<?php
$responses = array();
foreach (array('?arm=1', '', '?arm=1', '') as $query) {
    $response = false;
    for ($attempt = 0; $attempt < 100; $attempt++) {
        $response = @file_get_contents('http://127.0.0.1:18971/' . $query);
        if ($response !== false) { break; }
        usleep(20000);
    }
    if ($response === false) { fwrite(STDERR, "error: server did not respond\\n"); exit(2); }
    $decoded = json_decode($response, true);
    if (!is_array($decoded)) { fwrite(STDERR, "error: invalid server response\\n"); exit(3); }
    $responses[] = $decoded;
}
echo json_encode($responses);
""")
    server_results = []
    for armed in (False, True):
        label = 'armed' if armed else 'baseline'
        server = execute(common + ["-e", f"MEMINFO_ALLOC_PROBE_MODE={mode}", "-e",
                                  f"MEMINFO_ALLOC_PROBE_LOG=/results/server-{label}.allocator.log",
                                  "-e", f"ARMED_SEQUENCE={int(armed)}",
                                  image, "sh", "-c", """
set -eu
php -n -d extension=/src/extension/modules/meminfo.so -d extension=/results/oom_probe.so \
    -d memory_limit=64M -S 127.0.0.1:18971 /results/router.php >/results/server.log 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true' EXIT
php -n /results/client.php
"""])
        (output / f"server-client-{label}.stdout").write_text(server.stdout)
        (output / f"server-client-{label}.stderr").write_text(server.stderr)
        # Preserve each server log before starting the next container.
        (output / f"server-{label}.log").write_text((output / "server.log").read_text())
        try:
            responses = json.loads(server.stdout)
            lifecycle_ok = server.returncode == 0 and len(responses) == 4
            lifecycle_ok &= len({row["pid"] for row in responses}) == 1
            lifecycle_ok &= all(not row["before"]["active"] and not row["before"]["custom_heap"]
                                for row in responses)
            lifecycle_ok &= all(row["after"]["owns_handlers"] == armed for row in responses[::2])
            lifecycle_ok &= all(not row["after"]["active"] for row in responses[1::2])
        except (ValueError, TypeError, KeyError):
            responses = None
            lifecycle_ok = False
        server_results.append({"valid": bool(lifecycle_ok), "exit": server.returncode,
                               "responses": responses})
    lifecycle_ok = all(result["valid"] for result in server_results)
    if lifecycle_ok:
        # Zend retains a request-dependent cache even without our hook. Compare
        # matched idle requests, allowing one native 2 MiB chunk of placement noise.
        lifecycle_ok &= all(custom["real"] <= native["real"] + 2 * 1024 * 1024
                            for native, custom in zip(server_results[0]["responses"][1::2],
                                                      server_results[1]["responses"][1::2]))
    record("repeated-request-cleanup", lifecycle_ok,
           baseline=server_results[0], monitored=server_results[1])

    passed = all(row["passed"] for row in rows)
    report = {"image": image, "backend": backend, "gate_passed": passed, "results": rows,
              "scope": "NTS CLI and CLI-server, PHP 7.4-8.1; no FPM/JIT/debug-build proof"}
    (output / "semantics.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Allocator semantic gate: {'PASS' if passed else 'FAIL'}; results: {output}", flush=True)
    return 0 if passed else 1
