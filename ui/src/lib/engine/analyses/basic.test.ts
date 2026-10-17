import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import type { DumpStore } from '../store'
import { InvalidQueryError, query, ReferencePathFinder, referrers, summarize, topChildren, type QueryFilter } from './basic'
import { buildPredecessors, rootDistances } from './graph'
import { TopK } from './topk'

interface Expected {
  items: number
  summary: Array<{ type: string; count: number; self_size: number }>
  topChildren: Array<{ id: string; children: number }>
  queries: Array<{ name: string; filters: Record<string, { operator: '=' | '~'; value: string | number }>; limit: number; ids: string[] }>
  referencePaths: Array<{ label: string; target: string; paths: Array<{ root: string; length: number; names: string[] }> }>
}

const FIXTURES = ['basic', 'leak-before', 'leak-after']

function load(name: string): { store: DumpStore; expected: Expected } {
  const dir = new URL('../../../../tests/fixtures/parity/', import.meta.url)
  const bytes = new Uint8Array(readFileSync(new URL(`${name}.json`, dir)))
  const parser = new DumpParser(bytes.length)
  parser.push(bytes)
  return {
    store: parser.finish(),
    expected: JSON.parse(readFileSync(new URL(`${name}.expected.json`, dir), 'utf8')) as Expected,
  }
}

function parseText(text: string): DumpStore {
  const parser = new DumpParser()
  parser.push(new TextEncoder().encode(text))
  return parser.finish()
}

describe.each(FIXTURES)('matches the frozen baselines of the former PHP analyzer on %s', (name) => {
  const { store, expected } = load(name)

  it('has the same items', () => {
    expect(store.count).toBe(expected.items)
  })

  it('summarizes like SummaryCreator', () => {
    const rows = summarize(store)
    const asPhp = rows.map((row) => ({ type: row.key, count: row.count, self_size: row.selfSize }))

    // PHP leaves equal counts in no particular order: compare as sets, then the order of counts.
    expect([...asPhp].sort((a, b) => a.type.localeCompare(b.type))).toEqual([...expected.summary].sort((a, b) => a.type.localeCompare(b.type)))
    expect(rows.map((row) => row.count)).toEqual(expected.summary.map((row) => row.count))
  })

  it('finds the top children like TopSearcher', () => {
    const rows = topChildren(store, expected.topChildren.length)

    expect(rows.map((row) => ({ id: store.address(row.index), children: row.uniqueKeys }))).toEqual(expected.topChildren)
  })

  it('queries like QueryExecutor', () => {
    for (const expectedQuery of expected.queries) {
      const filters: QueryFilter[] = Object.entries(expectedQuery.filters).map(([attribute, filter]) => ({
        attribute: attribute as QueryFilter['attribute'],
        operator: filter.operator,
        value: String(filter.value),
      }))
      const result = query(store, filters, 0, expectedQuery.limit)

      expect(result.indices.map((index) => store.address(index)), expectedQuery.name).toEqual(expectedQuery.ids)
    }
  })

  it('finds the same reference paths as GraphBrowser (roots and lengths)', () => {
    const finder = new ReferencePathFinder(store, buildPredecessors(store))

    for (const target of expected.referencePaths) {
      const index = store.findAddress(target.target)
      const { paths } = finder.find(index, 10_000)
      // GraphBrowser counts the `<self>` edge as a path of length 1.
      const found = paths
        .map((path) => ({ root: store.address(path.root), length: path.self ? 1 : path.steps.length - 1 }))
        .sort((a, b) => a.root.localeCompare(b.root))

      expect(found, target.label).toEqual(target.paths.map(({ root, length }) => ({ root, length })))
    }
  })
})

describe('reference paths', () => {
  // root ─a→ mid ─b→ leaf ; other root ─c→ leaf ; leaf ─back→ mid (cycle)
  const store = parseText(
    '{"items":{' +
      '"0x10":{"type":"array","size":"72","symbol_name":"r1","is_root":true,"frame":"<GLOBAL>","children":{"a":"0x20"}},' +
      '"0x20":{"type":"array","size":"72","is_root":false,"children":{"b":"0x30"}},' +
      '"0x30":{"type":"array","size":"72","is_root":false,"children":{"back":"0x20"}},' +
      '"0x40":{"type":"array","size":"72","symbol_name":"r2","is_root":true,"frame":"f()","children":{"c":"0x30"}}' +
      '}}',
  )
  const preds = buildPredecessors(store)

  it('returns one shortest path per root, nearest first, each walking real edges', () => {
    const { paths, rootsReaching } = new ReferencePathFinder(store, preds).find(2)

    expect(rootsReaching).toBe(2)
    expect(paths.map((path) => store.address(path.root))).toEqual(['0x40', '0x10'])
    for (const path of paths) {
      for (let at = 1; at < path.steps.length; at++) {
        const step = path.steps[at]!
        expect(store.edgeTarget[step.edge]).toBe(step.item)
        expect(store.childStart[path.steps[at - 1]!.item]! <= step.edge && step.edge < store.childStart[path.steps[at - 1]!.item + 1]!).toBe(true)
      }
      expect(path.steps.at(-1)?.item).toBe(2)
    }
    expect(paths[1]?.steps.map((step) => (step.edge < 0 ? '' : store.edgeNameText(step.edge)))).toEqual(['', 'a', 'b'])
  })

  it('reports a root target as <self>', () => {
    const { paths } = new ReferencePathFinder(store, preds).find(0)

    expect(paths).toEqual([{ root: 0, self: true, steps: [{ item: 0, edge: -1 }] }])
  })

  it('lists referrers', () => {
    expect(referrers(preds, 2).map((r) => store.address(r.source)).sort()).toEqual(['0x20', '0x40'])
  })

  it('measures the distance from the nearest root', () => {
    const { distance, unreachable } = rootDistances(store)

    expect([...distance]).toEqual([0, 1, 1, 0])
    expect(unreachable).toBe(0)
  })
})

describe('query', () => {
  const { store } = load('basic')

  it('treats is_root as a real boolean', () => {
    const roots = query(store, [{ attribute: 'is_root', operator: '=', value: 'false' }], 0, 1_000_000)

    expect(roots.indices.every((index) => !store.isRoot(index))).toBe(true)
    expect(roots.total).toBeGreaterThan(0)
  })

  it('pages and counts all matches', () => {
    const all = query(store, [{ attribute: 'type', operator: '=', value: 'string' }], 0, 1_000_000)
    const page = query(store, [{ attribute: 'type', operator: '=', value: 'string' }], 5, 3)

    expect(page.total).toBe(all.total)
    expect(page.indices).toEqual(all.indices.slice(5, 8))
  })

  it('rejects an invalid regex', () => {
    expect(() => query(store, [{ attribute: 'class', operator: '~', value: '(' }])).toThrow(InvalidQueryError)
  })
})

describe('TopK', () => {
  it('keeps the best scores, earlier index first on ties', () => {
    const top = new TopK(3)
    for (const [score, index] of [[5, 0], [9, 1], [5, 2], [7, 3], [9, 4], [1, 5]] as const) top.push(score, index)

    expect(top.result()).toEqual([
      { score: 9, index: 1 },
      { score: 9, index: 4 },
      { score: 7, index: 3 },
    ])
  })
})
