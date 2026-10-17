import { existsSync, mkdirSync, statSync } from 'node:fs'
import { generateDump } from '../scripts/gen-dump.ts'

/**
 * Synthetic dumps for the memory tests, generated once into a git-ignored
 * directory and reused while they exist.
 */
export const GENERATED = new URL('./.generated/', import.meta.url)

export const DUMPS = {
  /** ~300 MB, ~2M items: the responsiveness and heap-budget case. */
  large: { file: 'large.json', requests: 245_000, fanout: 4 },
  /** > 512 MiB: past V8's string limit, where Blob.text()/JSON.parse fail. Opt-in. */
  huge: { file: 'huge.json', requests: 460_000, fanout: 4 },
} as const

export function dumpPath(name: keyof typeof DUMPS): string {
  return new URL(DUMPS[name].file, GENERATED).pathname
}

export default async function globalSetup(): Promise<void> {
  mkdirSync(GENERATED, { recursive: true })
  const wanted: Array<keyof typeof DUMPS> = process.env.MEMINFO_E2E_HUGE === '1' ? ['large', 'huge'] : ['large']
  for (const name of wanted) {
    const path = dumpPath(name)
    if (existsSync(path) && statSync(path).size > 0) continue
    const { requests, fanout } = DUMPS[name]
    const result = await generateDump(path, { requests, fanout, dangling: false })
    console.info(`[meminfo-ui:e2e] generated ${name}: ${result.items} items, ${(result.bytes / 1048576).toFixed(0)} MiB`)
  }
}
