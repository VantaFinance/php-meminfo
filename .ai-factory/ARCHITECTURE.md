# Architecture: Structured Modules by Technical Component

## Overview and rationale

The repository groups related behavior by runtime component: a native PHP extension, a PHP command-line trigger, and a browser analysis application. These components have distinct execution environments and share documented interfaces rather than a common application framework. This organization fits the existing project and keeps platform-specific concerns close to their implementation.

## Structure and communication

```text
extension/    Zend integration, traversal, dump writing, process triggers, tests
cli/          command-line process dump trigger
ui/           browser UI, analysis engine, worker, tests and report tooling
doc/          user and developer documentation
tools/        reproductions and diagnostic utilities
```

- The extension uses PHP/Zend APIs and writes a JSON dump; the UI reads that format through its parser and worker.
- The CLI invokes the extension's process-dump functionality. Signals and files coordinate with the target process.
- Optional S3 upload belongs to the extension build and is guarded by its build configuration.
- UI presentation communicates with the worker through the existing worker protocol; keep analysis in the engine rather than tying it to components.

## Dependency and compatibility rules

- Keep the dump format and process-trigger behavior compatible across producers and consumers; update documentation and fixtures when changing a contract.
- Keep platform and PHP-version differences behind existing compatibility checks. Avoid assumptions that break supported PHP versions or ZTS builds.
- Keep dump analysis local to the browser and avoid introducing a server dependency for core analysis.
- Respect C allocation ownership, safe points and error handling when working with Zend state or signals.

## Code organization

Follow existing component boundaries for new work. Adapt changes to the current structure and avoid unrelated structural rewrites. Keep interfaces between components explicit and exercise contract changes with relevant extension and UI tests.

## Examples

- A new dump field belongs in the extension writer and must be handled by the UI parser and analysis code when consumed there.
- A new analysis belongs in `ui/src/lib/engine/analyses/`, with results exposed through the worker before presentation in a Svelte component.

## Anti-patterns

- Calling application PHP code while traversing the heap or performing complex work in a signal handler.
- Coupling the browser engine to UI components or silently changing the dump contract.
- Moving platform-specific behavior into unrelated components without a clear interface.
