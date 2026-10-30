# User working preference

When executing an authorized plan, continue through all its tasks and required verification without stopping at an intermediate result. Pause only for a genuine blocker or a required approval; keep plan progress accurate.

## Project overview

PHP memory inspection extension with a process-dump CLI and a local browser analyzer. See `.ai-factory/DESCRIPTION.md`.

## Project structure

- `extension/` — C extension, Zend traversal, dump writer and PHPT tests.
- `cli/` — PHP `dump-meminfo` command.
- `ui/` — Svelte and TypeScript analyzer, worker, unit and browser tests.
- `doc/` — usage and development documentation.
- `tools/` — reproduction and diagnostic tools.
- `Makefile`, `CMakeLists.txt`, `Dockerfile`, `.github/workflows/` — build and CI infrastructure.

## Key entry points and context

| File | Purpose |
| --- | --- |
| `extension/meminfo.c` | PHP extension entry points |
| `extension/config.m4` | Extension build configuration |
| `cli/bin/dump-meminfo` | Process dump command |
| `ui/src/main.ts` | Browser UI entry point |
| `README.md` | Project usage and documentation links |
| `doc/development.md` | Build and test guidance |
| `.ai-factory/DESCRIPTION.md` | Project description |
| `.ai-factory/ARCHITECTURE.md` | Architecture guidance |
| `.ai-factory/rules/base.md` | Detected project conventions |

Run shell operations as separate commands when they have independent purposes; for example, run `git checkout master` and then `git pull origin master` separately.
