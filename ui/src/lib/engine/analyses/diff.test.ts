import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import type { DumpStore } from '../store'
import { compareWithBaseline, reduceToBaseline } from './diff'

function load(name: string): DumpStore {
  const bytes = new Uint8Array(readFileSync(new URL(`../../../../tests/fixtures/parity/${name}.json`, import.meta.url)))
  const parser = new DumpParser(bytes.length)
  parser.push(bytes)
  return parser.finish()
}

describe('compareWithBaseline', () => {
  const baseline = reduceToBaseline(load('leak-before'), 'leak-before.json')
  const rows = compareWithBaseline(baseline, load('leak-after'))

  it('keeps only aggregates of the baseline', () => {
    expect(baseline.items).toBe(400)
    expect(JSON.stringify(baseline).length).toBeLessThan(2_000)
  })

  it('puts the leaking class among the most grown, by 150 instances', () => {
    const request = rows.find((row) => row.isClass && row.key === 'Request')

    expect(request).toMatchObject({ before: { count: 50 }, after: { count: 200 }, countDelta: 150, isNew: false })
    expect(rows.slice(0, 3).map((row) => row.key)).toContain('Request')
  })

  it('does not report the steady array as growing', () => {
    expect(rows.every((row) => row.countDelta >= 0)).toBe(true)
    expect(rows[0]!.countDelta).toBeGreaterThanOrEqual(rows.at(-1)!.countDelta)
  })

  it('marks new and gone types', () => {
    const synthetic = compareWithBaseline(
      { name: 'b', items: 1, totalSize: 16, rows: [{ key: 'Old', isClass: true, count: 1, selfSize: 72 }] },
      load('leak-before'),
    )

    expect(synthetic.find((row) => row.key === 'Old')).toMatchObject({ isGone: true, countDelta: -1 })
    expect(synthetic.find((row) => row.key === 'Request')).toMatchObject({ isNew: true, countDelta: 50 })
  })
})
