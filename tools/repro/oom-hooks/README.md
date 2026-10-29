# OOM interception prototypes

Research fixtures and reproducible regression images. Production now uses call
observation; the allocator interposition experiment below remains rejected.

`allocator_probe.c` installs Zend custom allocation handlers that delegate to
the original heap. At a configurable threshold they set a pending flag and
request a VM interrupt. The interrupt calls the existing internal
`meminfo_dump` function, temporarily lifting the internal memory limit.
The callbacks themselves never traverse PHP values. Dumping is skipped during
compilation, exceptions and shutdown. `notify` mode only logs; `opcode` mode
checks at each opcode as an experimental alternative.

`exit_probe.c` is a Linux `LD_PRELOAD` interposer for libc `_exit`. It writes a
small marker using a pre-opened descriptor and exits through `exit_group`,
preserving the supplied status. It does not call PHP or attempt a heap dump.
It observes all `_exit` calls, so a marker alone does not identify an OOM.

The alternative `MEMINFO_ALLOC_PROBE_MODE=internal` in the standalone probe
checks used bytes after internal calls and leaves Zend MM unchanged. Production
uses the same notification principle with user-call observers and the existing
fd writer/interrupt pipeline, rather than the probe's stream/PHP-call dump path.

## Run

The Linux image must contain `cc`, `php-config`, an NTS PHP build and meminfo at
`/src/extension/modules/meminfo.so`. Run from the repository root:

```sh
python3 tools/repro/oom-hooks/run.py \
  --image meminfo:ci-ubuntu-php81 \
  --output /tmp/oom-hooks-patched

python3 tools/repro/oom-hooks/run.py \
  --image meminfo:ci-fix-php8.1 --expected-oom-exit 255 \
  --output /tmp/oom-hooks-legacy

python3 tools/repro/oom-hooks/benchmark.py /tmp/oom-hooks-patched

# Mandatory native allocator equivalence gate (currently exits 1: backend rejected).
python3 tools/repro/oom-hooks/run.py \
  --image meminfo:ci-ubuntu-php81 --semantics \
  --output /tmp/oom-hooks-semantics

# Accepted narrower backend: semantic gate and explicit JIT capture checks.
python3 tools/repro/oom-hooks/run.py --image meminfo:ci-ubuntu-php81 \
  --backend internal --semantics --output /tmp/oom-internal-semantics
python3 tools/repro/oom-hooks/run.py --image meminfo:ci-ubuntu-php81 \
  --backend internal --jit --output /tmp/oom-internal-jit
```

Output directories must be empty. The runner compiles all three C files with
warnings treated as errors and retains scripts, stdout/stderr, allocator/exit
logs, JSON dumps and `results.json`. It checks actual process status, shutdown
behavior, dump structure and a known PHP object in each successful dump.
Six standalone exit controls check status preservation, including truncation
of negative and greater-than-255 statuses. Containers have a 90-second timeout.

### Allocator feasibility gate

`--semantics` builds the same prototypes and runs differential controls in fresh
processes: explicit reclamation, automatic reclamation at memory_limit,
allocate/free churn, changed limits, unlimited-to-finite limits, reclamation
after disarm, handler identity/rearm, rejection of USE_ZEND_ALLOC=0, automatic
RSHUTDOWN cleanup and repeated requests in a single CLI-server process. It
retains scripts, output and logs plus `semantics.json`. Exit 1 indicates failed
equivalence; it is not converted into a passing test for a known limitation.

On patched Ubuntu PHP 8.1 and legacy Alpine PHP 7.4, **8/10 checks pass and two
fail**: explicit GC and automatic GC at the limit. After releasing 15,000
512-byte strings, native gc_mem_caches reclaims about 9.6 MB (real usage drops
from 12 MiB to 4 MiB); with the prototype it reclaims 0. With memory_limit=16M,
allocating a 9 MiB string after that release succeeds natively but causes OOM
with the prototype (exit 1 on patched PHP, 255 on legacy PHP).

Disarming restores reclamation; that does not preserve application behavior
while the hook is armed. Zend's custom-heap flag disables zend_mm_gc, including
automatic collection during native limit checks. Request cleanup and handler
restoration pass in the tested CLI server, but FPM/debug/JIT are unverified.
Comparisons allow one native 2 MiB chunk of placement noise and 10% reclaimed
byte variation for GC fixtures, rather than requiring identical bookkeeping
between processes. Repeated-request checks compare matching native requests,
since Zend itself retains a request-dependent chunk cache.

**Do not integrate this allocator backend into meminfo.** The gate rejects it
despite the earlier successful capture fixtures. Temporarily unhooking just
while dumping would not fix gc_mem_caches or automatic collection during the
rest of the application's execution. The existing fcall-observer threshold
remains the narrower option; no safe allocation-hook adapter has been validated.

### Pinned production regressions

The repository-root build contexts below include the real extension and PHPT
fixtures. `Dockerfile` pins the legacy image digest and security-backported PHP
package version. `verify-engine.php` independently checks native behavior and
records PHP, module and source hashes; a fixture changing class fails the build.

```sh
docker build --build-arg RUNTIME=legacy-runtime \
  -f tools/repro/oom-hooks/Dockerfile -t meminfo:oom-legacy .
docker build --build-arg RUNTIME=backport-runtime \
  -f tools/repro/oom-hooks/Dockerfile -t meminfo:oom-backport .
docker run --rm meminfo:oom-backport sh -c \
  'REPORT_EXIT_STATUS=1 NO_INTERACTION=1 php run-tests.php -n -d extension=/src/extension/modules/meminfo.so tests/dump-on_limit.phpt tests/dump-on_threshold*.phpt'

docker build --build-arg RUNTIME=fpm-runtime \
  -f tools/repro/oom-hooks/Dockerfile -t meminfo:oom-fpm .
docker run --rm meminfo:oom-fpm php -n /src/tools/repro/oom-hooks/fpm.php
```

`fault_probe.c` is compiled into the test images only. It injects bailout,
unrestorable limit and ENOSPC cases in `dump-on_threshold_cleanup.phpt`; it is
never part of meminfo.so. The FPM check exercises reuse, per-request reset,
native GC, fatal termination and worker replacement in a private single-worker
pool. The `oom-engines` CI job runs these fixtures on Linux AMD64; local
verification was on ARM64. Remote CI results must be inspected separately.

For S3, first build the existing CRT image, then optionally build the exact EOL
mitigation source against that toolchain:

```sh
docker build --build-arg PHP_VERSION=8.4 -f tools/repro/s3/Dockerfile -t meminfo-s3:oom-8.4 .
docker build -f tools/repro/oom-hooks/Dockerfile.s3-backport -t meminfo-s3:oom-backport .
bash tools/repro/s3/run-tests.sh meminfo-s3:oom-8.4 'tests/dump-s3_*.phpt'
bash tools/repro/s3/run-tests.sh meminfo-s3:oom-backport 'tests/dump-s3_*.phpt'
```

The source-built PHP pins commit `1d7dec178a094e6e75b229ce22754045f8c6070a` and
must demonstrate immediate exit before its image is accepted. Early-upload
tests synchronize on confirmed success or an accepted connection with no
server response; neither outcome relies on a sleep race.

### Alternative backend results

The internal-call prototype passes 10/10 semantic checks on legacy PHP 7.4 and
patched PHP 8.1, and 14/14 capture checks on patched PHP 8.1 with JIT enabled.
The interrupt hook must be installed at module startup: installing it at arm
time missed callbacks in already generated JIT stubs. This observation also
guides the production hook lifetime.

Its microbenchmark median was 26.84 ms versus 25.79 ms baseline (+4.1%); this is
the standalone probe workload, not a production overhead guarantee. The
backend still misses pure allocation loops without function calls and a single
internal call that fails before returning. These are explicit unsupported
capture cases, not silently accepted successful dumps.

## Observed results

Tests ran on Linux ARM64 containers. Ubuntu PHP 8.1.34-9 with the security
backport terminates OOM through `_exit(1)` without PHP shutdown. Older Alpine
PHP 7.4 and 8.1 terminate with status 255 and run shutdown callbacks.

| Scenario, memory_limit=16M | Result |
| --- | --- |
| Repeated 1 KiB allocations, threshold 12 MiB | Valid early dump, original OOM status preserved |
| Straight-line 32 KiB allocations | Both interrupt and opcode modes produced valid dumps in these fixtures |
| Growing integer array / realloc | Valid early dump with the known object |
| Single 64 MiB allocation | Notification, no early dump: no VM callback before fatal termination |
| Normal completion, before/after threshold | Status 0; custom allocator removed after disarming |
| Patched PHP `_exit(1)` | Preload marker captured, status 1 preserved |

Initial legacy runs produced a small dump during shutdown after the single
large allocation failed. The explicit shutdown guard excludes that late dump.

The Ubuntu microbenchmark executes one million `str_repeat(128)`/`strlen`
iterations without crossing the threshold. Five measured samples after a
warmup gave median 25.42 ms baseline, 27.75 ms allocator/interrupt (+9.1%), and
65.90 ms opcode mode (+159.2%). These are workload-specific measurements,
excluding Docker launch and actual dumping; they are not application estimates.
The follow-up run after adding lifecycle diagnostics measured 25.04 ms baseline,
28.10 ms allocator (+12.2%) and 65.38 ms opcode (+161.1%); run-to-run variation
does not change the failed allocator-semantic gate.

## Limits

This proves early threshold capture, not a reliable dump at the failing
allocation. A single large allocation can bypass the last safe opportunity.
The threshold is conservative (requested realloc size is not net heap growth).
The interrupt does not guarantee a callback between every pair of allocations.
Walking the heap directly inside an allocator or `_exit` hook remains unsafe:
Zend data structures may be in the middle of mutation.

The allocator experiment only supports NTS and assumes exclusive ownership of
its installed handlers until disarm; it rejects an existing custom allocator.
It checks handler identity before removal, but concurrent replacement/chaining
is unsupported; an ownership-loss diagnostic is not a recovery mechanism.
Its stream is expected to be a plain local file. It temporarily removes the
internal limit while dumping, so OS memory exhaustion is still possible.
The allocator backend's FPM, ZTS, other allocator extensions, concurrent hook
replacement, S3 transport and x86 behavior have not been validated. Opcode
handlers can inhibit JIT. Production FPM/JIT evidence above applies to the
call-observation backend only.
The exit interposer depends on dynamic libc symbol binding and a writable,
owner-only regular log file. Logging is best effort; regular-file writes can
block. Neither prototype is suitable for production activation as-is.
