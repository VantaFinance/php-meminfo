/**
 * Deterministic synthetic meminfo dump, written with the extension's exact byte
 * layout (field order, irregular whitespace, string-typed sizes), so that
 * large-dump tests need no PHP.
 *
 *   node scripts/gen-dump.ts <out.json> [--requests=N] [--fanout=N] [--dangling]
 *
 * Shape: a function frame and <GLOBAL> with a few roots, plus a leak — the
 * static LeakyCache::$requests array holding N Request objects, each with a
 * payload string, an int id, a tags array of `fanout` strings and a back
 * reference to the cache array (a cycle). Every Request emits 4 + fanout items.
 */

import { createWriteStream, type WriteStream } from 'node:fs'
import { once } from 'node:events'

export interface GenOptions {
  requests: number
  fanout: number
  dangling: boolean
}

const FLUSH_AT = 1 << 20

class Writer {
  private chunks: string[] = []
  private size = 0
  bytes = 0
  private readonly out: WriteStream

  constructor(out: WriteStream) {
    this.out = out
  }

  async write(text: string): Promise<void> {
    this.chunks.push(text)
    this.size += text.length
    if (this.size >= FLUSH_AT) {
      await this.flush()
    }
  }

  async flush(): Promise<void> {
    if (this.chunks.length === 0) {
      return
    }
    const data = this.chunks.join('')
    this.chunks = []
    this.size = 0
    this.bytes += Buffer.byteLength(data)
    if (!this.out.write(data)) {
      await once(this.out, 'drain')
    }
  }
}

/** Mirrors meminfo_zval_dump(): the separator before an item depends on the previous one. */
class DumpWriter {
  private first = true
  items = 0
  edges = 0
  private nextAddress = 0x104000000
  private readonly w: Writer

  constructor(w: Writer) {
    this.w = w
  }

  address(): string {
    const value = this.nextAddress
    this.nextAddress += 0x48
    return '0x' + value.toString(16)
  }

  async header(): Promise<void> {
    await this.w.write(
      '{\n  "header" : {\n    "memory_usage" : 2097152' +
        ',\n    "memory_usage_real" : 4194304' +
        ',\n    "peak_memory_usage" : 2097152' +
        ',\n    "peak_memory_usage_real" : 4194304' +
        '\n  },\n  "items": {\n',
    )
  }

  private async open(address: string, type: string, size: number, root?: { symbol?: string; frame: string }): Promise<void> {
    let text = this.first ? '' : '\n    },\n'
    this.first = false
    text += `    "${address}" : {\n        "type" : "${type}",\n        "size" : "${size}",\n`
    if (root !== undefined) {
      if (root.symbol !== undefined) {
        text += `        "symbol_name" : "${root.symbol}",\n`
      }
      text += `        "is_root" : true,\n        "frame" : "${root.frame}"\n`
    } else {
      text += '        "is_root" : false\n'
    }
    this.items++
    await this.w.write(text)
  }

  async scalar(address: string, type: string, size: number, root?: { symbol?: string; frame: string }): Promise<void> {
    await this.open(address, type, size, root)
    await this.w.write('\n')
  }

  async array(address: string, children: Array<[string, string]>, root?: { symbol?: string; frame: string }): Promise<void> {
    await this.open(address, 'array', 72, root)
    await this.w.write(',\n' + this.children(children))
  }

  async object(address: string, className: string, handle: number, children: Array<[string, string]>, root?: { symbol?: string; frame: string }): Promise<void> {
    await this.open(address, 'object', 72, root)
    await this.w.write(`,\n        "class" : "${className}",\n        "object_handle" : "${handle}",\n` + this.children(children))
  }

  private children(children: Array<[string, string]>): string {
    this.edges += children.length
    const body = children.map(([name, child]) => `            "${name}":"${child}"`).join(',\n')
    return '        "children" : {\n' + body + '\n        }\n'
  }

  async end(): Promise<void> {
    // Same closing the extension writes; it assumes at least one item.
    await this.w.write('\n    }\n}\n}\n')
  }
}

export async function generateDump(path: string, options: GenOptions): Promise<{ bytes: number; items: number; edges: number }> {
  const out = createWriteStream(path)
  const w = new Writer(out)
  const d = new DumpWriter(w)

  await d.header()

  // A function frame with a couple of locals, as a request handler would have.
  await d.scalar(d.address(), 'string', 17, { symbol: 'local', frame: 'handle_request()' })
  await d.scalar(d.address(), 'int', 16, { symbol: 'id', frame: 'handle_request()' })

  // Steady state in <GLOBAL>: not growing.
  const steady = d.address()
  const steadyChildren: Array<[string, string]> = []
  const steadyItems: string[] = []
  for (let i = 0; i < 10; i++) {
    const child = d.address()
    steadyChildren.push([String(i), child])
    steadyItems.push(child)
  }
  await d.array(steady, steadyChildren, { symbol: 'notLeaking', frame: '<GLOBAL>' })
  for (const child of steadyItems) {
    await d.scalar(child, 'string', 22)
  }

  // The leak: LeakyCache::$requests keeps every Request.
  const cache = d.address()
  const requestAddresses: string[] = []
  for (let i = 0; i < options.requests; i++) {
    requestAddresses.push(d.address())
  }
  await d.array(
    cache,
    requestAddresses.map((address, i) => [String(i), address]),
    { symbol: 'LeakyCache::requests', frame: '<CLASS_STATIC_MEMBER>' },
  )

  for (let i = 0; i < options.requests; i++) {
    const payload = d.address()
    const id = d.address()
    const tags = d.address()
    const tagItems: string[] = []
    for (let t = 0; t < options.fanout; t++) {
      tagItems.push(d.address())
    }

    await d.object(requestAddresses[i]!, 'Request', i + 1, [
      ['payload', payload],
      ['tags', tags],
      ['id', id],
      ['cache', cache],
    ])
    await d.scalar(payload, 'string', 80)
    await d.scalar(id, 'int', 16)
    await d.array(tags, tagItems.map((address, t) => [String(t), address]))
    for (const tag of tagItems) {
      await d.scalar(tag, 'string', 24)
    }
  }

  if (options.dangling) {
    await d.array(d.address(), [['missing', '0xdead0000']], { symbol: 'broken', frame: '<GLOBAL>' })
  }

  await d.end()
  await w.flush()
  out.end()
  await once(out, 'finish')

  return { bytes: w.bytes, items: d.items, edges: d.edges }
}

function parseArgs(argv: string[]): { path: string; options: GenOptions } {
  const [path, ...rest] = argv
  if (path === undefined) {
    throw new Error('usage: node scripts/gen-dump.ts <out.json> [--requests=N] [--fanout=N] [--dangling]')
  }
  const options: GenOptions = { requests: 1000, fanout: 4, dangling: false }
  for (const arg of rest) {
    const [key, value] = arg.replace(/^--/, '').split('=')
    if (key === 'requests' && value !== undefined) {
      options.requests = Number(value)
    } else if (key === 'fanout' && value !== undefined) {
      options.fanout = Number(value)
    } else if (key === 'dangling') {
      options.dangling = true
    } else {
      throw new Error(`unknown option ${arg}`)
    }
  }
  return { path, options }
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const { path, options } = parseArgs(process.argv.slice(2))
  const started = performance.now()
  const result = await generateDump(path, options)
  console.info(
    `[meminfo-ui:gen-dump] ${path}: ${result.items} items, ${result.edges} edges, ` +
      `${(result.bytes / 1048576).toFixed(1)} MiB in ${((performance.now() - started) / 1000).toFixed(1)} s`,
  )
}
