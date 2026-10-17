import { createReadStream, mkdirSync, statSync } from 'node:fs'
import { Readable } from 'node:stream'
import { describe, expect, it } from 'vitest'
import { generateDump } from '../../../scripts/gen-dump.ts'
import { computeDominators } from './analyses/dominators'
import { buildPredecessors } from './analyses/graph'
import { findSuspects } from './analyses/suspects'
import { parseDumpStream } from './parser'

/**
 * Throughput and memory of the parser on a large synthetic dump. Opt-in:
 *   MEMINFO_PERF=1 npx vitest run src/lib/engine/parser.perf.test.ts
 */
describe.runIf(process.env.MEMINFO_PERF === '1')('parser performance', () => {
  it('parses a ~370 MB dump at a few hundred MB/s with a compact store', async () => {
    const dir = new URL('../../../tests-e2e/.generated/', import.meta.url)
    mkdirSync(dir, { recursive: true })
    const path = new URL('perf-dump.json', dir).pathname
    const generated = await generateDump(path, { requests: 300_000, fanout: 4, dangling: false })
    const bytes = statSync(path).size

    const before = process.memoryUsage()
    const started = performance.now()
    const stream = Readable.toWeb(createReadStream(path, { highWaterMark: 2 * 1024 * 1024 })) as ReadableStream<Uint8Array>
    const store = await parseDumpStream(stream, { expectedBytes: bytes })
    const seconds = (performance.now() - started) / 1000
    const after = process.memoryUsage()

    const stats = store.stats()
    const report = {
      mib: Math.round(bytes / 1048576),
      items: stats.items,
      seconds: Number(seconds.toFixed(2)),
      mbPerSecond: Math.round(bytes / 1e6 / seconds),
      storeBytesPerItem: Math.round(stats.bytes / stats.items),
      heapDeltaMiB: Math.round((after.heapUsed - before.heapUsed) / 1048576),
      arrayBuffersMiB: Math.round(after.arrayBuffers / 1048576),
    }
    const graphStarted = performance.now()
    const preds = buildPredecessors(store)
    const dominators = computeDominators(store, preds)
    const report2 = findSuspects(store, dominators)
    const graphSeconds = (performance.now() - graphStarted) / 1000
    console.info('[meminfo-ui:perf]', { ...report, graphSeconds: Number(graphSeconds.toFixed(2)), passes: dominators.passes, topSuspect: store.item(report2.suspects[0]!.index).symbol })

    expect(stats.items).toBe(generated.items)
    expect(stats.edges).toBe(generated.edges)
    expect(report.storeBytesPerItem).toBeLessThan(70)
    expect(report.heapDeltaMiB).toBeLessThan(100)
  }, 300_000)
})
