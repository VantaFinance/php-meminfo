import { createLogger } from '../../log'
import type { DumpStore } from '../store'
import { summarize } from './basic'

const log = createLogger('diff')

/**
 * Growth between two dumps of the same process — the documented way to find a
 * leak: what keeps growing from one dump to the next.
 *
 * Only aggregates of the baseline are kept, so loading it costs a parse and a
 * few kilobytes, not a second store. Addresses are never compared across dumps:
 * they mean nothing outside the dump they come from. Retained sizes are not
 * compared either — under sharing they move around without anything growing.
 */

export interface BaselineRow {
  key: string
  isClass: boolean
  count: number
  selfSize: number
}

export interface Baseline {
  name: string
  items: number
  totalSize: number
  rows: BaselineRow[]
}

export function reduceToBaseline(store: DumpStore, name: string): Baseline {
  const rows = summarize(store)
  const baseline: Baseline = {
    name,
    items: store.count,
    totalSize: rows.reduce((sum, row) => sum + row.selfSize, 0),
    rows: rows.map(({ key, isClass, count, selfSize }) => ({ key, isClass, count, selfSize })),
  }
  log.info('baseline reduced', { items: baseline.items, rows: baseline.rows.length })
  return baseline
}

export interface GrowthRow {
  key: string
  isClass: boolean
  before: { count: number; selfSize: number }
  after: { count: number; selfSize: number }
  countDelta: number
  sizeDelta: number
  /** Absent from the baseline. */
  isNew: boolean
  /** Absent from the current dump. */
  isGone: boolean
}

/** Every type/class of either dump, the most grown first. */
export function compareWithBaseline(baseline: Baseline, store: DumpStore): GrowthRow[] {
  const key = (row: { key: string; isClass: boolean }): string => (row.isClass ? 'class:' : 'type:') + row.key
  const before = new Map(baseline.rows.map((row) => [key(row), row]))
  const rows: GrowthRow[] = []

  for (const row of summarize(store)) {
    const old = before.get(key(row))
    before.delete(key(row))
    rows.push({
      key: row.key,
      isClass: row.isClass,
      before: { count: old?.count ?? 0, selfSize: old?.selfSize ?? 0 },
      after: { count: row.count, selfSize: row.selfSize },
      countDelta: row.count - (old?.count ?? 0),
      sizeDelta: row.selfSize - (old?.selfSize ?? 0),
      isNew: old === undefined,
      isGone: false,
    })
  }
  for (const old of before.values()) {
    rows.push({
      key: old.key,
      isClass: old.isClass,
      before: { count: old.count, selfSize: old.selfSize },
      after: { count: 0, selfSize: 0 },
      countDelta: -old.count,
      sizeDelta: -old.selfSize,
      isNew: false,
      isGone: true,
    })
  }

  return rows.sort((a, b) => b.countDelta - a.countDelta || b.sizeDelta - a.sizeDelta || a.key.localeCompare(b.key))
}
