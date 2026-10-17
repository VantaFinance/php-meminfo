import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { DumpParseError, DumpParser, parseDumpStream } from './parser'
import type { DumpStore, ItemView } from './store'

const fixture = (name: string): Uint8Array =>
  new Uint8Array(readFileSync(new URL(`../../../tests/fixtures/${name}.json`, import.meta.url)))

const PARITY = ['parity/basic', 'parity/leak-before', 'parity/leak-after']
const UI_ONLY = ['ui-only/invalid-utf8', 'ui-only/nul-keys', 'ui-only/dup-private', 'ui-only/lost-root']

function parseChunks(bytes: Uint8Array, chunks: Uint8Array[]): DumpStore {
  const parser = new DumpParser(bytes.length)
  for (const chunk of chunks) {
    parser.push(chunk)
  }
  return parser.finish()
}

function parseWhole(bytes: Uint8Array): DumpStore {
  return parseChunks(bytes, [bytes])
}

function parseText(text: string): DumpStore {
  return parseWhole(new TextEncoder().encode(text))
}

/** Order-independent, comparable form of every item. */
function canonical(store: DumpStore): Map<string, unknown> {
  const items = new Map<string, unknown>()
  for (let i = 0; i < store.count; i++) {
    const item: ItemView = store.item(i)
    items.set(item.address, {
      type: item.type,
      size: item.size,
      isRoot: item.isRoot,
      frame: item.frame,
      symbol: item.symbol,
      className: item.className,
      handle: item.handle,
      children: item.children?.map((child) => `${child.name}→${child.targetAddress}`).sort(),
    })
  }
  return items
}

/** The same form, built from JSON.parse (valid only without duplicate keys). */
function canonicalFromJson(bytes: Uint8Array): Map<string, unknown> {
  const dump = JSON.parse(new TextDecoder().decode(bytes)) as {
    items: Record<string, Record<string, unknown> & { children?: Record<string, string> }>
  }
  const items = new Map<string, unknown>()
  for (const [address, raw] of Object.entries(dump.items)) {
    items.set(address, {
      type: raw.type,
      size: Number(raw.size),
      isRoot: raw.is_root,
      frame: raw.frame,
      symbol: raw.symbol_name,
      className: raw.class,
      handle: raw.object_handle === undefined ? undefined : Number(raw.object_handle),
      children: raw.children === undefined ? undefined : Object.entries(raw.children).map(([name, target]) => `${name}→${target}`).sort(),
    })
  }
  return items
}

function findBySymbol(store: DumpStore, symbol: string): ItemView | undefined {
  for (let i = 0; i < store.count; i++) {
    const item = store.item(i)
    if (item.symbol === symbol) return item
  }
  return undefined
}

function findByClass(store: DumpStore, className: string): ItemView[] {
  const found: ItemView[] = []
  for (let i = 0; i < store.count; i++) {
    if (store.className(i) === className) found.push(store.item(i))
  }
  return found
}

describe('DumpParser on real dumps', () => {
  it.each(PARITY)('%s matches JSON.parse item by item', (name) => {
    const bytes = fixture(name)
    const store = parseWhole(bytes)

    expect(canonical(store)).toEqual(canonicalFromJson(bytes))
    expect(store.danglingEdges).toBe(0)
    expect(store.duplicateAddresses).toBe(0)
    expect(store.header.memory_usage).toBeGreaterThan(0)
  })

  it.each([...PARITY, ...UI_ONLY])('%s gives the same result split at every byte offset', (name) => {
    const bytes = fixture(name)
    const expected = canonical(parseWhole(bytes))

    // Every offset on the small fixtures; a prime stride on the 64–208 KB leak ones.
    const step = bytes.length > 30_000 ? 97 : 1
    for (let offset = 1; offset < bytes.length; offset += step) {
      const store = parseChunks(bytes, [bytes.subarray(0, offset), bytes.subarray(offset)])
      if (store.count !== expected.size) {
        throw new Error(`split at ${offset}: ${store.count} items instead of ${expected.size}`)
      }
    }
    // Item counts above are cheap; compare everything for a sample of offsets.
    for (const offset of [1, 2, 7, 100, 1001, Math.floor(bytes.length / 2), bytes.length - 2]) {
      expect(canonical(parseChunks(bytes, [bytes.subarray(0, offset), bytes.subarray(offset)]))).toEqual(expected)
    }
  })

  it.each(UI_ONLY)('%s gives the same result in chunks of 1 to 17 bytes', (name) => {
    const bytes = fixture(name)
    const expected = canonical(parseWhole(bytes))

    for (let size = 1; size <= 17; size++) {
      const chunks: Uint8Array[] = []
      for (let at = 0; at < bytes.length; at += size) {
        chunks.push(bytes.subarray(at, at + size))
      }
      expect(canonical(parseChunks(bytes, chunks))).toEqual(expected)
    }
  })

  it('keeps every numeric and string key, including negative and PHP_INT_MAX', () => {
    const store = parseWhole(fixture('parity/basic'))
    const mixed = findBySymbol(store, 'mixedKeys')

    expect(mixed?.children?.map((child) => child.name)).toEqual(['5', 'k', '-3', '9223372036854775807'])
  })

  it('reads objects, statics and fiber frames', () => {
    const store = parseWhole(fixture('parity/basic'))

    const child = findByClass(store, 'Child')
    expect(child).toHaveLength(1)
    expect(child[0]?.children?.map((c) => c.name).sort()).toEqual(['extra', 'priv', 'prot', 'pub'])
    expect(findBySymbol(store, 'Registry::items')?.frame).toBe('<CLASS_STATIC_MEMBER>')
    expect(findBySymbol(store, 'insideFiber')?.isRoot).toBe(true)
    expect(findByClass(store, 'Suit')).toHaveLength(1)
  })

  it('keeps duplicate private property names as separate edges', () => {
    const store = parseWhole(fixture('ui-only/dup-private'))
    const [object] = findByClass(store, 'DupChild')

    const secrets = object?.children?.filter((child) => child.name === 'secret') ?? []
    expect(secrets).toHaveLength(2)
    expect(secrets[0]?.target).not.toBe(secrets[1]?.target)
  })

  it('keeps keys truncated at NUL as separate edges', () => {
    const store = parseWhole(fixture('ui-only/nul-keys'))
    const keys = findBySymbol(store, 'keys')

    expect(keys?.children?.map((child) => child.name)).toEqual(['a', 'a', 'b'])
  })

  it('decodes escapes and replaces invalid UTF-8', () => {
    const store = parseWhole(fixture('ui-only/invalid-utf8'))
    const keys = findBySymbol(store, 'keys')

    expect(keys?.children?.map((child) => child.name)).toEqual([
      'plain',
      'bad��',
      'ctl\u0001\u001f',
      'quote"back\\slash',
      'café',
    ])
  })

  it('reflects the lost-root quirk: later root holders emit no item', () => {
    const store = parseWhole(fixture('ui-only/lost-root'))

    expect(findBySymbol(store, 'local')?.className).toBe('stdClass')
    expect(findBySymbol(store, 'alsoHeld')).toBeUndefined()
    expect(findBySymbol(store, 'Holder::kept')).toBeUndefined()
  })
})

describe('DumpParser on edge cases', () => {
  it('accepts minified JSON and any field order', () => {
    const store = parseText(
      '{"items":{"0x10":{"children":{"x":"0x20"},"is_root":true,"frame":"<GLOBAL>","symbol_name":"a","size":"72","type":"array"},' +
        '"0x20":{"size":16,"type":"int","is_root":false}},"header":{"memory_usage":1}}',
    )

    expect(store.count).toBe(2)
    expect(store.item(0)).toMatchObject({ address: '0x10', type: 'array', size: 72, isRoot: true, symbol: 'a' })
    expect(store.item(0).children).toEqual([{ name: 'x', target: 1, targetAddress: '0x20' }])
    expect(store.item(1).size).toBe(16)
    expect(store.header.memory_usage).toBe(1)
  })

  it('reads an empty dump', () => {
    expect(parseText('{"header":{},"items":{}}').count).toBe(0)
  })

  it('counts dangling children instead of failing', () => {
    const store = parseText('{"items":{"0x10":{"type":"array","size":"72","is_root":true,"frame":"<GLOBAL>","children":{"gone":"0xdead"}}}}')

    expect(store.danglingEdges).toBe(1)
    expect(store.item(0).children?.[0]?.target).toBe(-1)
  })

  it('skips unknown fields, including nested ones', () => {
    const store = parseText('{"items":{"0x10":{"type":"int","size":"16","future":{"a":[1,{"b":null}],"c":"d"},"is_root":false}}}')

    expect(store.count).toBe(1)
    expect(store.item(0)).toMatchObject({ type: 'int', size: 16 })
  })

  it('combines surrogate pairs and replaces lone surrogates', () => {
    const store = parseText(
      '{"items":{"0x10":{"type":"array","size":"72","is_root":true,"frame":"f","children":{"\\ud83d\\ude00":"0x10","\\ud83dx":"0x10"}}}}',
    )

    expect(store.item(0).children?.map((child) => child.name)).toEqual(['😀', '�x'])
  })

  it('handles a 200 000-level chain without recursion', () => {
    const depth = 200_000
    const parts = ['{"items":{']
    for (let i = 0; i < depth; i++) {
      const address = '0x' + (0x1000 + i * 8).toString(16)
      const next = '0x' + (0x1000 + (i + 1) * 8).toString(16)
      const children = i + 1 < depth ? `,"children":{"next":"${next}"}` : ''
      parts.push(`${i === 0 ? '' : ','}"${address}":{"type":"array","size":"72","is_root":${i === 0}${children}}`)
    }
    parts.push('}}')
    const store = parseText(parts.join(''))

    expect(store.count).toBe(depth)
    expect(store.edgeCount).toBe(depth - 1)
    expect(store.danglingEdges).toBe(0)
    // Every item points at the next one: catches columns falling out of step as they grow.
    for (let i = 0; i + 1 < depth; i++) {
      if (store.childStart[i + 1]! - store.childStart[i]! !== 1 || store.edgeTarget[store.childStart[i]!] !== i + 1) {
        throw new Error(`item ${i} does not point at item ${i + 1}`)
      }
    }
  })

  it('reports truncated and malformed input with an offset', () => {
    expect(() => parseText('{"items":{"0x10":{"type":"int"')).toThrow(DumpParseError)
    expect(() => parseText('{"items":{"0x10":{"type":"int","is_root":tru}}}')).toThrow(/invalid literal/)
    expect(() => parseText('{"items":{"not-an-address":{}}}')).toThrow(/not an address/)
    expect(() => parseText('{"items":{]}')).toThrow(/unbalanced/)
  })
})

describe('parseDumpStream', () => {
  it('reads a ReadableStream and reports progress', async () => {
    const bytes = fixture('parity/leak-after')
    const chunks = [bytes.subarray(0, 1000), bytes.subarray(1000, 50_000), bytes.subarray(50_000)]
    const stream = new ReadableStream<Uint8Array>({
      start(controller) {
        for (const chunk of chunks) controller.enqueue(chunk)
        controller.close()
      },
    })
    const progress: number[] = []

    const store = await parseDumpStream(stream, { expectedBytes: bytes.length, onProgress: (p) => progress.push(p.bytes) })

    expect(store.count).toBe(canonicalFromJson(bytes).size)
    expect(progress).toEqual([1000, 50_000, bytes.length])
  })

  it('stops when cancelled', async () => {
    const controller = new AbortController()
    controller.abort()
    const stream = new ReadableStream<Uint8Array>({ start: (c) => c.enqueue(new Uint8Array([0x7b])) })

    await expect(parseDumpStream(stream, { signal: controller.signal })).rejects.toThrow(/cancelled/)
  })
})
