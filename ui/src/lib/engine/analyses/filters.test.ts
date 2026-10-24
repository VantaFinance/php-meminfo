import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import type { DumpStore } from '../store'
import { computeDominators } from './dominators'
import { ignoredEdges, InvalidFilterError } from './filters'
import { buildPredecessors, rootDistances } from './graph'
import { findSuspects } from './suspects'

const address = (i: number): string => '0x' + (0x1000 + i * 8).toString(16)

/**
 * $logger (Logger) ->records [] Entry(2), and $app (App) ->entries [] the same
 * Entry: two holders. A third item, Orphan, is held only by Logger->last.
 */
function store(): DumpStore {
  const items = [
    `"${address(0)}":{"type":"object","size":"100","symbol_name":"logger","is_root":true,"frame":"<GLOBAL>","class":"Logger","children":{"records":"${address(2)}","last":"${address(3)}"}}`,
    `"${address(1)}":{"type":"object","size":"100","symbol_name":"app","is_root":true,"frame":"<GLOBAL>","class":"App","children":{"entries":"${address(2)}"}}`,
    `"${address(2)}":{"type":"object","size":"5000","is_root":false,"class":"Entry","children":{}}`,
    `"${address(3)}":{"type":"object","size":"7000","is_root":false,"class":"Orphan","children":{}}`,
  ]
  const parser = new DumpParser()
  parser.push(new TextEncoder().encode(`{"items":{${items.join(',')}}}`))
  return parser.finish()
}

describe('ignoredEdges', () => {
  it('applies nothing without filters', () => {
    expect(ignoredEdges(store(), [])).toBeNull()
  })

  it('makes an item held only through an ignored edge unreachable, and drops it from the suspects', () => {
    const dump = store()
    const preds = buildPredecessors(dump)
    const ignored = ignoredEdges(dump, [{ kind: 'edge', holder: 'Logger', name: 'last' }])
    const dominators = computeDominators(dump, preds, ignored)
    expect(dominators.idom[3]).toBe(-1)
    expect(rootDistances(dump, ignored).distance[3]).toBe(-1)
    const suspects = findSuspects(dump, dominators, 10)
    expect(suspects.suspects.map((suspect) => suspect.index)).not.toContain(3)
    expect(dominators.retained[0]).toBe(100)
  })

  it('shows the other holder once a class of holders is ignored', () => {
    const dump = store()
    const preds = buildPredecessors(dump)
    // Shared by Logger and App: dominated by the virtual root.
    expect(computeDominators(dump, preds).idom[2]).toBe(dump.count)
    const dominators = computeDominators(dump, preds, ignoredEdges(dump, [{ kind: 'holders', pattern: '^Logger$' }]))
    expect(dominators.idom[2]).toBe(1)
    expect(dominators.retained[1]).toBe(5100)
  })

  it('restores the original view when the filters are removed', () => {
    const dump = store()
    const preds = buildPredecessors(dump)
    const before = computeDominators(dump, preds)
    computeDominators(dump, preds, ignoredEdges(dump, [{ kind: 'holders', pattern: 'Logger' }]))
    const after = computeDominators(dump, preds, ignoredEdges(dump, []))
    expect([...after.idom]).toEqual([...before.idom])
  })

  it('rejects an invalid pattern', () => {
    expect(() => ignoredEdges(store(), [{ kind: 'holders', pattern: '(' }])).toThrow(InvalidFilterError)
  })
})
