# PHP Meminfo — browser UI

Opens a meminfo dump in the browser and points at likely memory leaks. No backend: the file is read
and analyzed client-side, and the built app's Content Security Policy forbids any request off the
page. User-facing documentation is in the repository's README ("Analyzing a dump in the browser").

## Layout

| Path | What lives there |
|---|---|
| `src/lib/engine/` | Pure TypeScript, no DOM: the streaming `parser.ts`, the columnar `DumpStore` (`store.ts`), and `analyses/` (summary, top children, query, reference paths, dominators, suspects, diff) |
| `src/lib/worker/` | `session.ts` (one dump, every request on it), `dump.worker.ts`, the page-side `client.ts`, and `protocol.ts` — the only types that cross the boundary |
| `src/lib/state/` | App state on Svelte 5 runes; everything from the worker is `$state.raw` |
| `src/lib/components/` | `VirtualTable` (windowed, paged, height-capped), load/progress/recovery screens, `Inspector`, `AddressLink`, `views/` |
| `src/lib/components/graph/` | The Graph view's drawing: `elements.ts` (result → Cytoscape elements, style, layout; pure, tested headless) and `GraphCanvas.svelte` (one Cytoscape instance, destroyed with the canvas) |
| `scripts/gen-dump.ts` | Synthetic dumps with the extension's exact byte layout |
| `tests/fixtures/` | Real dumps: `parity/` (also readable by the PHP analyzer, with `*.expected.json` baselines) and `ui-only/` (quirks the PHP analyzer cannot open). They are regenerated through the extension, so the layout of older extensions is covered by a dump kept inline in `parser.test.ts` |
| `tests-e2e/` | Playwright: app flow, privacy, memory budget, crash recovery, graph |

## Commands

```bash
npm ci
npm run dev          # dev server
npm run build        # static build in dist/ (relative URLs; serve it, file:// does not work)
npm run check        # svelte-check + tsc (app, node scripts, unit tests, e2e)
npm test             # engine tests (Vitest)
npm run test:e2e     # build + Playwright (Chromium; `npx playwright install chromium` once)

MEMINFO_PERF=1 npx vitest run src/lib/engine/parser.perf.test.ts   # parser throughput on ~370 MB
MEMINFO_E2E_HUGE=1 npm run test:e2e                                  # adds a dump past 512 MiB
node scripts/gen-dump.ts out.json --requests=100000 --fanout=4      # a synthetic dump
```

From the repository root, `make ui-install`, `make ui-dev`, `make ui-check`, `make ui-test`,
`make ui-e2e` do the same; `make ui-fixtures` regenerates the fixtures and the PHP analyzer
baselines (PHP 8.1+ with meminfo, and `make install` for the analyzer).

## Deployment

`.github/workflows/ui.yaml` runs the checks, Vitest and Playwright on every change under `ui/`, and on
`master` publishes the very `dist/` the e2e tests ran against to GitHub Pages. Asset URLs are
relative (`base: './'`), so the repository sub-path needs no configuration, and the CSP is a `<meta>`
tag, so it holds without custom headers, which Pages cannot set. Once per repository: Settings → Pages → Source:
"GitHub Actions".

## Rules that keep a large dump from taking the tab down

- The dump is parsed and analyzed in a Web Worker; the page holds only the rows on screen.
- The file is streamed (`file.stream()`), never read whole: `Blob.text()` silently returns `""`
  past 512 MiB and `JSON.parse` loses duplicate `children` keys.
- No per-item objects: typed-array columns, interned names, children as CSR.
- One worker per dump, terminated to free it; a baseline is reduced to totals in a worker of its own.
- Data from the worker is `$state.raw`; tables are windowed and fetch pages.
- Graphs are built in the worker (`analyses/subgraph.ts`) and only a bounded subgraph crosses to the
  page: at most 300 items per request, 600 on screen with expansions.
- Cytoscape.js (~140 KB gzip, more than the rest of the app) is its own chunk, loaded only when the
  Graph view first opens; the instance is destroyed when the view closes.
- `npm run test:e2e` checks the main-thread heap, the worker's typed-array memory, three
  open/close cycles and cancellation, and that no request leaves the page.
