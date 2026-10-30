# vanta/dump-meminfo

The `dump-meminfo` command: dumps the memory of an already running PHP process to a JSON file and
prints its path. It is a thin wrapper over `meminfo_dump_process()` of the
[meminfo extension](https://github.com/VantaFinance/php-meminfo).

```bash
composer require --dev vanta/dump-meminfo
vendor/bin/dump-meminfo <pid> [--dir=DIR] [--signal=SIG] [--timeout=SEC] [--force] [--count=N] [--interval=TIME] [--wait-upload]
```

`--count=3 --interval=5m` takes a series of dumps and prints their paths in order. Opened together in
the browser UI, they show what grows at every step and the objects left behind between the first two.

`--wait-upload` is for a target that uploads its dumps to S3 (`meminfo.dump_upload`): it waits for the
upload and prints the `s3://` location instead of the local path, or fails with the target's reason.

The command needs the meminfo extension loaded, and so does the process it dumps. Install the
extension with [PIE](https://github.com/php/pie):

```bash
pie install vanta/meminfo
```

Open the dump in the browser UI (nothing is uploaded): https://vantafinance.github.io/php-meminfo/

This repository is a read-only split of the `cli/` directory of
[VantaFinance/php-meminfo](https://github.com/VantaFinance/php-meminfo). Send issues and pull
requests there.
