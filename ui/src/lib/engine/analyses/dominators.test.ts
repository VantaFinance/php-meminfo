import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import type { DumpStore } from '../store'
import { computeDominators } from './dominators'
import { buildPredecessors } from './graph'
import { dominatorChain, findSuspects } from './suspects'

interface Graph {
  roots: Set<number>
  edges: Array<[number, number]>
  dangling: number[]
  sizes: number[]
}

const address = (i: number): string => '0x' + (0x1000 + i * 8).toString(16)

function storeOf(graph: Graph): DumpStore {
  const parts: string[] = []
  graph.sizes.forEach((size, i) => {
    const children = graph.edges.filter(([from]) => from === i).map(([, to], k) => `"e${k}":"${address(to)}"`)
    for (const [k, from] of graph.dangling.entries()) {
      if (from === i) children.push(`"gone${k}":"0xdead${k}0"`)
    }
    const root = graph.roots.has(i) ? `"symbol_name":"r${i}","is_root":true,"frame":"<GLOBAL>"` : '"is_root":false'
    parts.push(`"${address(i)}":{"type":"array","size":"${size}",${root},"children":{${children.join(',')}}}`)
  })
  const parser = new DumpParser()
  parser.push(new TextEncoder().encode(`{"items":{${parts.join(',')}}}`))
  return parser.finish()
}

/** Dominator sets by the textbook dataflow equations; idom = the closest strict dominator. */
function bruteForceIdom(graph: Graph): number[] {
  const n = graph.sizes.length
  const root = n
  const preds: number[][] = Array.from({ length: n + 1 }, () => [])
  for (const [from, to] of graph.edges) preds[to]!.push(from)
  for (const r of graph.roots) preds[r]!.push(root)

  // Reachability from the virtual root.
  const reachable = new Set<number>([root])
  const queue = [root]
  while (queue.length > 0) {
    const node = queue.shift()!
    const next = node === root ? [...graph.roots] : graph.edges.filter(([f]) => f === node).map(([, t]) => t)
    for (const t of next) {
      if (!reachable.has(t)) {
        reachable.add(t)
        queue.push(t)
      }
    }
  }

  const all = new Set(reachable)
  const dom = new Map<number, Set<number>>()
  for (const node of reachable) dom.set(node, node === root ? new Set([root]) : new Set(all))
  let changed = true
  while (changed) {
    changed = false
    for (const node of reachable) {
      if (node === root) continue
      const meets: Array<Set<number>> = []
      for (const p of preds[node]!) {
        if (reachable.has(p)) meets.push(dom.get(p)!)
      }
      const [first, ...rest] = meets
      const next = new Set<number>([...(first ?? [])].filter((x) => rest.every((set) => set.has(x))))
      next.add(node)
      if (next.size !== dom.get(node)!.size) {
        dom.set(node, next)
        changed = true
      }
    }
  }

  const idom: number[] = []
  for (let v = 0; v < n; v++) {
    if (!reachable.has(v)) {
      idom.push(-1)
      continue
    }
    const strict = [...dom.get(v)!].filter((d) => d !== v)
    idom.push(strict.reduce((best, d) => (dom.get(d)!.size > dom.get(best)!.size ? d : best)))
  }
  return idom
}

/** Small deterministic PRNG. */
function random(seed: number): () => number {
  let state = seed >>> 0
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0
    return state / 2 ** 32
  }
}

describe('computeDominators', () => {
  it('matches the dataflow definition on 300 random graphs', () => {
    const rand = random(42)
    for (let trial = 0; trial < 300; trial++) {
      const n = 2 + Math.floor(rand() * 11)
      const graph: Graph = { roots: new Set(), edges: [], dangling: [], sizes: [] }
      for (let i = 0; i < n; i++) {
        graph.sizes.push(1 + Math.floor(rand() * 100))
        if (rand() < 0.25) graph.roots.add(i)
        if (rand() < 0.1) graph.dangling.push(i)
      }
      if (graph.roots.size === 0) graph.roots.add(0)
      for (let from = 0; from < n; from++) {
        for (let to = 0; to < n; to++) {
          if (rand() < 0.2) graph.edges.push([from, to]) // includes self-loops and cycles
        }
      }

      const store = storeOf(graph)
      const dominators = computeDominators(store, buildPredecessors(store))
      const expected = bruteForceIdom(graph).map((d) => (d === n ? store.count : d))

      expect([...dominators.idom.subarray(0, n)], `trial ${trial}`).toEqual(expected)

      // Retained size of the virtual root = everything reachable.
      const reachableSize = graph.sizes.reduce((sum, size, i) => sum + (expected[i] === -1 ? 0 : size), 0)
      expect(dominators.retained[store.count]).toBe(reachableSize)
    }
  })

  it('walks a 200 000-item chain without recursion', () => {
    const n = 200_000
    const graph: Graph = { roots: new Set([0]), edges: [], dangling: [], sizes: new Array<number>(n).fill(1) }
    for (let i = 0; i + 1 < n; i++) graph.edges.push([i, i + 1])
    const parts: string[] = []
    for (let i = 0; i < n; i++) {
      const child = i + 1 < n ? `,"children":{"next":"${address(i + 1)}"}` : ''
      parts.push(`"${address(i)}":{"type":"array","size":"1","is_root":${i === 0}${child}}`)
    }
    const parser = new DumpParser()
    parser.push(new TextEncoder().encode(`{"items":{${parts.join(',')}}}`))
    const store = parser.finish()

    const dominators = computeDominators(store, buildPredecessors(store))

    expect(dominators.retained[0]).toBe(n)
    expect(dominators.idom[n - 1]).toBe(n - 2)
    expect(findSuspects(store, dominators, 10).suspects.map((s) => s.index)).toEqual([0])
  })
})

describe('findSuspects', () => {
  it('flags data shared by several roots and collapses chains', () => {
    // r0 → shared ; r1 → shared ; shared → a → b (a chain of 3)
    const graph: Graph = {
      roots: new Set([0, 1]),
      edges: [[0, 2], [1, 2], [2, 3], [3, 4]],
      dangling: [],
      sizes: [10, 10, 100, 100, 100],
    }
    const store = storeOf(graph)
    const dominators = computeDominators(store, buildPredecessors(store))
    const report = findSuspects(store, dominators)

    const shared = report.suspects.find((s) => s.index === 2)
    expect(shared).toMatchObject({ shared: true, retained: 300, dominated: 3 })
    // 3 and 4 are folded into 2: their dominators retain barely more.
    expect(report.suspects.map((s) => s.index)).not.toContain(4)
    expect(report.sharedItems).toBe(1)
    expect(dominatorChain(dominators, 4)).toEqual([2, 3, 4])
  })

  it('points at the leaking static array in the leak fixture', () => {
    const bytes = new Uint8Array(readFileSync(new URL('../../../../tests/fixtures/parity/leak-after.json', import.meta.url)))
    const parser = new DumpParser(bytes.length)
    parser.push(bytes)
    const store = parser.finish()
    const report = findSuspects(store, computeDominators(store, buildPredecessors(store)))

    const top = store.item(report.suspects[0]!.index)
    expect(top.symbol).toBe('LeakyCache::requests')
    expect(top.frame).toBe('<CLASS_STATIC_MEMBER>')

    const requests = report.classes.find((c) => c.key === 'Request')
    expect(requests).toMatchObject({ isClass: true, instances: 200, topMost: 200 })
    expect(report.frames[0]?.frame).toBe('<CLASS_STATIC_MEMBER>')
    expect(report.totalSize).toBeGreaterThan(0)
  })
})
