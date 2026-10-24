import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import type { DumpStore } from '../store'
import { computeDominators } from './dominators'
import { findFindings, type Finding } from './findings'
import { buildPredecessors } from './graph'
import { findSuspects } from './suspects'

interface Spec {
  size: number
  type?: string
  className?: string
  root?: boolean
  children?: number[]
  fields?: Record<string, string | number>
}

const address = (i: number): string => '0x' + (0x1000 + i * 8).toString(16)

function storeOf(items: Spec[], header: Record<string, number> = {}): DumpStore {
  const parts = items.map((item, i) => {
    const fields: string[] = [`"type":"${item.type ?? (item.className ? 'object' : 'array')}"`, `"size":"${item.size}"`]
    fields.push(item.root ? `"symbol_name":"r${i}","is_root":true,"frame":"<GLOBAL>"` : '"is_root":false')
    if (item.className) fields.push(`"class":"${item.className}"`)
    for (const [key, value] of Object.entries(item.fields ?? {})) {
      fields.push(`"${key}":${typeof value === 'number' ? value : JSON.stringify(value)}`)
    }
    fields.push(`"children":{${(item.children ?? []).map((child, k) => `"c${k}":"${address(child)}"`).join(',')}}`)
    return `"${address(i)}":{${fields.join(',')}}`
  })
  const parser = new DumpParser()
  parser.push(new TextEncoder().encode(`{"header":${JSON.stringify(header)},"items":{${parts.join(',')}}}`))
  return parser.finish()
}

function findingsOf(store: DumpStore): Finding[] {
  const preds = buildPredecessors(store)
  const dominators = computeDominators(store, preds)
  const suspects = findSuspects(store, dominators, 100)
  return findFindings(store, dominators, suspects, preds)
}

function kinds(findings: Finding[]): string[] {
  return findings.map((finding) => finding.kind)
}

describe('findFindings', () => {
  it('reports nothing on a dump where nothing stands out', () => {
    const items: Spec[] = [{ size: 100, root: true, children: [1, 2] }, { size: 100 }, { size: 100 }]
    expect(findingsOf(storeOf(items))).toEqual([])
  })

  it('finds an accumulation point: one holder of many small items', () => {
    const items: Spec[] = [{ size: 56, root: true, children: Array.from({ length: 200 }, (_, k) => k + 1) }]
    for (let k = 0; k < 200; k++) items.push({ size: 100 })
    const findings = findingsOf(storeOf(items))
    const accumulation = findings.find((finding) => finding.kind === 'accumulation-point')
    expect(accumulation).toMatchObject({ index: 0, count: 200, severity: 'high' })
  })

  it('finds a class retaining most of the dump', () => {
    const items: Spec[] = [{ size: 56, root: true, children: [1, 2] }]
    items.push({ size: 5000, className: 'Big' }, { size: 5000, className: 'Big' })
    const finding = findingsOf(storeOf(items)).find((f) => f.kind === 'dominant-class')
    expect(finding).toMatchObject({ className: 'Big', count: 2, index: -1 })
  })

  it('finds a cycle and what it keeps alive', () => {
    const items: Spec[] = [
      { size: 56, root: true, children: [1] },
      { size: 100, className: 'Node', children: [2, 3] },
      { size: 100, className: 'Node', children: [1, 4] },
      { size: 5000 },
      { size: 5000 },
    ]
    const cycle = findingsOf(storeOf(items)).find((f) => f.kind === 'cycle')
    expect(cycle).toMatchObject({ count: 2 })
    expect(cycle!.bytes).toBe(10_200)
  })

  it('groups closures by where they were declared', () => {
    const items: Spec[] = [{ size: 56, root: true, children: Array.from({ length: 25 }, (_, k) => k + 1) }]
    for (let k = 0; k < 25; k++) {
      items.push({ size: 344, className: 'Closure', fields: { function: '{closure}', declared_at: '/app/src/Listener.php:12' } })
    }
    const finding = findingsOf(storeOf(items)).find((f) => f.kind === 'closures')
    expect(finding).toMatchObject({ count: 25 })
    expect(finding!.title).toContain('/app/src/Listener.php:12')
  })

  it('finds arrays that are mostly empty slots', () => {
    const capacity = 65_536
    const items: Spec[] = [
      { size: 56, root: true, children: [1] },
      { size: 56 + capacity * 40, children: [2], fields: { capacity, used: capacity } },
      { size: 0, type: 'int' },
    ]
    const finding = findingsOf(storeOf(items)).find((f) => f.kind === 'sparse-arrays')
    expect(finding).toMatchObject({ index: 1, count: 1 })
    expect(finding!.bytes).toBe((capacity - 1) * 40)
  })

  it('reports memory no item accounts for, only with allocation sizes', () => {
    const items: Spec[] = [{ size: 1000, root: true }]
    expect(kinds(findingsOf(storeOf(items, { memory_usage: 64 << 20, size_model: 2 })))).toContain('unattributed')
    expect(kinds(findingsOf(storeOf(items, { memory_usage: 64 << 20 })))).not.toContain('unattributed')
  })

  it('reports a process much bigger than what PHP allocated', () => {
    const items: Spec[] = [{ size: 1000, root: true }]
    const big = { memory_usage: 8 << 20, memory_usage_real: 10 << 20, rss: 300 << 20 }
    expect(kinds(findingsOf(storeOf(items, big)))).toContain('outside-zend')
    expect(kinds(findingsOf(storeOf(items, { ...big, rss: 15 << 20 })))).not.toContain('outside-zend')
  })

  it('reports objects found alive with no root reaching them', () => {
    const parser = new DumpParser()
    parser.push(
      new TextEncoder().encode(
        '{"items":{"0x10":{"type":"array","size":"100","symbol_name":"g","is_root":true,"frame":"<GLOBAL>","children":{}},' +
          '"0x20":{"type":"object","size":"100","symbol_name":"#7","is_root":true,"frame":"<UNREACHABLE>","class":"CycleMember","children":{"other":"0x30"}},' +
          '"0x30":{"type":"object","size":"100","is_root":false,"class":"CycleMember","children":{"other":"0x20"}}}}',
      ),
    )
    const finding = findingsOf(parser.finish()).find((f) => f.kind === 'unreachable')
    expect(finding).toMatchObject({ count: 1, bytes: 200 })
  })

  it('finds duplicate strings by hash and the class holding them', () => {
    const items: Spec[] = [{ size: 56, root: true, children: Array.from({ length: 100 }, (_, k) => k + 1) }]
    for (let k = 0; k < 100; k++) items.push({ size: 64, className: 'Row', children: [101 + k] })
    for (let k = 0; k < 100; k++) items.push({ size: 20_000, type: 'string', fields: { hash: '00000000000000aa' } })
    const finding = findingsOf(storeOf(items)).find((f) => f.kind === 'duplicate-strings')
    expect(finding).toMatchObject({ count: 99, bytes: 99 * 20_000 })
    expect(finding!.hypothesis).toContain('held by Row')
  })

  it('ranks by severity, then by bytes', () => {
    const items: Spec[] = [{ size: 56, root: true, children: Array.from({ length: 200 }, (_, k) => k + 1) }]
    for (let k = 0; k < 200; k++) items.push({ size: 100, className: 'Entry' })
    const findings = findingsOf(storeOf(items, { memory_usage: 64 << 20, size_model: 2 }))
    const rank = { high: 0, medium: 1, low: 2 }
    for (let at = 1; at < findings.length; at++) {
      expect(rank[findings[at - 1]!.severity]).toBeLessThanOrEqual(rank[findings[at]!.severity])
    }
  })

  it('is stable on the leak fixture', () => {
    const parser = new DumpParser()
    parser.push(new Uint8Array(readFileSync(new URL('../../../../tests/fixtures/parity/leak-after.json', import.meta.url))))
    const findings = findingsOf(parser.finish())
    expect(findings.map((finding) => `${finding.severity} ${finding.kind}: ${finding.title}`)).toMatchSnapshot()
  })
})
