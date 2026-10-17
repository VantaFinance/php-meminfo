import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import type { DumpStore } from '../store'
import { ReferencePathFinder } from './basic'
import { computeDominators } from './dominators'
import { buildPredecessors } from './graph'
import { neighbourhoodSubgraph, pathsSubgraph } from './subgraph'

const address = (i: number): string => '0x' + (0x1000 + i * 8).toString(16)

/** items[i] = [isRoot, child indices]; a child index ≥ items.length is dangling. */
function storeOf(items: Array<[boolean, number[]]>): DumpStore {
  const parts = items.map(([root, children], i) => {
    const kids = children.map((child, k) => `"k${k}":"${address(child)}"`).join(',')
    const rootFields = root ? `"symbol_name":"r${i}","is_root":true,"frame":"<GLOBAL>"` : '"is_root":false'
    return `"${address(i)}":{"type":"array","size":"10",${rootFields},"children":{${kids}}}`
  })
  const parser = new DumpParser()
  parser.push(new TextEncoder().encode(`{"items":{${parts.join(',')}}}`))
  return parser.finish()
}

function tools(store: DumpStore) {
  const preds = buildPredecessors(store)
  return { preds, finder: new ReferencePathFinder(store, preds), dominators: computeDominators(store, preds) }
}

describe('pathsSubgraph', () => {
  it('merges one shortest path per root, and every edge is a real edge', () => {
    const bytes = new Uint8Array(readFileSync(new URL('../../../../tests/fixtures/parity/leak-after.json', import.meta.url)))
    const parser = new DumpParser(bytes.length)
    parser.push(bytes)
    const store = parser.finish()
    const { finder, dominators } = tools(store)
    const target = [...Array(store.count).keys()].find((i) => store.className(i) === 'Request')!

    const subgraph = pathsSubgraph(finder, dominators, target, 300)
    const expected = new Set(finder.find(target, 1000).paths.flatMap((path) => path.steps.map((step) => step.item)))

    expect(new Set(subgraph.nodes)).toEqual(expected)
    for (const edge of subgraph.edges) {
      expect(store.edgeTarget[edge.edge]).toBe(edge.target)
      expect(store.childStart[edge.source]! <= edge.edge && edge.edge < store.childStart[edge.source + 1]!).toBe(true)
    }
    expect(subgraph.truncated).toBe(false)
    expect(subgraph.bottlenecks.map((index) => store.item(index).symbol)).toEqual(['LeakyCache::requests'])
  })

  // r0 → a → t, r1 → b → t  (diamond: no single bottleneck)
  // r2 → c → d → t2         (chain: c and d are bottlenecks)
  const store = storeOf([
    [true, [2]], // 0 r0
    [true, [3]], // 1 r1
    [false, [4]], // 2 a
    [false, [4]], // 3 b
    [false, [4, 4, 99]], // 4 t: self-loop, dangling child
    [true, [6]], // 5 r2
    [false, [7]], // 6 c
    [false, [8, 6]], // 7 d → t2, and back to c (cycle)
    [false, []], // 8 t2
  ])
  const { finder, dominators } = tools(store)

  it('finds no bottleneck in a diamond and every link of a chain', () => {
    expect(pathsSubgraph(finder, dominators, 4, 100).bottlenecks).toEqual([])
    expect(pathsSubgraph(finder, dominators, 8, 100).bottlenecks.sort()).toEqual([5, 6, 7])
  })

  it('adds whole paths only and flags the cap', () => {
    const capped = pathsSubgraph(finder, dominators, 4, 3)

    expect(capped.truncated).toBe(true)
    expect(capped.nodes).toHaveLength(3) // target + one whole path (root, middle)
    expect(capped.totalRoots).toBe(2)
    expect(capped.edges).toHaveLength(2)
  })

  it('draws a root target alone', () => {
    const own = pathsSubgraph(finder, dominators, 0, 100)

    expect(own.nodes).toEqual([0])
    expect(own.edges).toEqual([])
  })
})

describe('neighbourhoodSubgraph', () => {
  // 0 → 1 → 2 → 3, 4 → 1
  const store = storeOf([
    [true, [1]],
    [false, [2]],
    [false, [3]],
    [false, []],
    [true, [1]],
  ])
  const { preds } = tools(store)

  it('follows children only, by hops', () => {
    expect(neighbourhoodSubgraph(store, preds, 1, 1, 100, 'children').nodes.sort()).toEqual([1, 2])
    expect(neighbourhoodSubgraph(store, preds, 1, 2, 100, 'children').nodes.sort()).toEqual([1, 2, 3])
  })

  it('follows parents and both directions, with every edge between included nodes', () => {
    expect(neighbourhoodSubgraph(store, preds, 1, 1, 100, 'parents').nodes.sort()).toEqual([0, 1, 4])
    const both = neighbourhoodSubgraph(store, preds, 1, 1, 100, 'both')
    expect(both.nodes.sort()).toEqual([0, 1, 2, 4])
    expect(both.edges.map((e) => `${e.source}-${e.target}`).sort()).toEqual(['0-1', '1-2', '4-1'])
  })

  it('stops at the cap', () => {
    const capped = neighbourhoodSubgraph(store, preds, 1, 3, 2, 'both')

    expect(capped.nodes).toHaveLength(2)
    expect(capped.truncated).toBe(true)
  })
})
