import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import type { DumpStore } from '../store'
import { computeDominators } from './dominators'
import { buildPredecessors, rootDistances } from './graph'
import { analyzeRetainers, instancesOf } from './retainers'

const address = (i: number): string => '0x' + (0x1000 + i * 8).toString(16)

interface Spec {
  className?: string
  root?: string
  children?: Array<[string, number]>
}

function storeOf(items: Spec[]): DumpStore {
  const parts = items.map((item, i) => {
    const fields = [`"type":"${item.className ? 'object' : 'array'}"`, '"size":"100"']
    fields.push(item.root ? `"symbol_name":"${item.root}","is_root":true,"frame":"<GLOBAL>"` : '"is_root":false')
    if (item.className) fields.push(`"class":"${item.className}"`)
    fields.push(`"children":{${(item.children ?? []).map(([name, to]) => `"${name}":"${address(to)}"`).join(',')}}`)
    return `"${address(i)}":{${fields.join(',')}}`
  })
  const parser = new DumpParser()
  parser.push(new TextEncoder().encode(`{"items":{${parts.join(',')}}}`))
  return parser.finish()
}

/**
 * Two leaks of Entry: three held by $cache[] (integer keys), two by
 * Registry->listeners[] under $registry.
 */
function twoLeaks(): DumpStore {
  return storeOf([
    { root: 'cache', children: [['0', 2], ['1', 3], ['2', 4]] },
    { root: 'registry', className: 'Registry', children: [['listeners', 5]] },
    { className: 'Entry' },
    { className: 'Entry' },
    { className: 'Entry' },
    { children: [['10', 6], ['11', 7]] },
    { className: 'Entry' },
    { className: 'Entry' },
  ])
}

function analyze(store: DumpStore, skip: RegExp | null = null) {
  const preds = buildPredecessors(store)
  return analyzeRetainers(store, rootDistances(store), computeDominators(store, preds), instancesOf(store, 'Entry', true), skip)
}

describe('analyzeRetainers', () => {
  it('puts two leaks with distinct paths in two clusters, integer keys collapsed', () => {
    const report = analyze(twoLeaks())
    expect(report.instances).toBe(5)
    expect(report.clusters.map((cluster) => [cluster.count, cluster.steps])).toEqual([
      [3, ['<GLOBAL> $cache array', '[] → Entry']],
      [2, ['<GLOBAL> $registry Registry', '->listeners → array', '[] → Entry']],
    ])
  })

  it('merges the paths into one tree with counts', () => {
    const { merged } = analyze(twoLeaks())
    expect(merged.count).toBe(5)
    expect(merged.children.map((node) => [node.step, node.count])).toEqual([
      ['<GLOBAL> $cache array', 3],
      ['<GLOBAL> $registry Registry', 2],
    ])
    expect(merged.children[1]!.children[0]!.children[0]).toMatchObject({ step: '[] → Entry', count: 2 })
  })

  it('groups immediate dominators by class and skips holders matching the pattern', () => {
    expect(analyze(twoLeaks()).dominators.map((group) => [group.key, group.members])).toEqual([
      ['array', 5],
    ])
    expect(analyze(twoLeaks(), /^array$/).dominators.map((group) => [group.key, group.members])).toEqual([
      ['(several roots, or a root itself)', 3],
      ['Registry', 2],
    ])
  })

  it('finds no instances of a class the dump does not have', () => {
    expect(analyze(storeOf([{ root: 'x' }])).instances).toBe(0)
  })
})
