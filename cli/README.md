# vanta/dump-meminfo

The `dump-meminfo` command: dumps the memory of an already running PHP process to a JSON file and
prints its path. It is a thin wrapper over `meminfo_dump_process()` of the
[meminfo extension](https://github.com/VantaFinance/php-meminfo).

```bash
composer require --dev vanta/dump-meminfo
vendor/bin/dump-meminfo <pid> [--dir=DIR] [--signal=SIG] [--timeout=SEC] [--force]
```

The command needs the meminfo extension loaded, and so does the process it dumps. Install the
extension with [PIE](https://github.com/php/pie):

```bash
pie install vanta/meminfo
```

Open the dump in the browser UI (nothing is uploaded): https://vantafinance.github.io/php-meminfo/

This repository is a read-only split of the `cli/` directory of
[VantaFinance/php-meminfo](https://github.com/VantaFinance/php-meminfo). Send issues and pull
requests there.
