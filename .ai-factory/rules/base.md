# Project base rules

> Conventions inferred from the existing codebase; edit as needed.

## Naming and structure

- C extension files and functions generally use lowercase snake_case and the `meminfo` prefix; keep public PHP API names consistent.
- TypeScript modules use descriptive names, generally camelCase; Svelte components use PascalCase.
- Keep Zend traversal and serialization in `extension/`, the process trigger in `cli/`, and browser analysis in `ui/`.

## Errors and control flow

- Follow the surrounding C ownership and cleanup conventions; propagate failures through existing PHP exception and error mechanisms.
- Prefer guard clauses and small helpers when they keep the main path readable. Preserve compatibility checks for supported PHP versions and platforms.

## Logging and tests

- Use existing error reporting and logging facilities rather than introducing a new logging framework.
- Add PHPT coverage for extension behavior, Vitest coverage for UI analysis, and Playwright coverage for relevant browser interactions. See `doc/development.md` and `ui/package.json` for commands.
