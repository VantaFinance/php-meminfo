import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { DumpParser } from '../engine/parser'
import type { DumpStore } from '../engine/store'
import { buildReport, parseSeverity, renderHtml } from './report'

function load(name: string): DumpStore {
  const parser = new DumpParser()
  parser.push(new Uint8Array(readFileSync(new URL(`../../../tests/fixtures/parity/${name}.json`, import.meta.url))))
  return parser.finish()
}

describe('buildReport', () => {
  it('passes without rules, and lists findings, classes and growth', () => {
    const report = buildReport('leak-after.json', load('leak-after'), { name: 'leak-before.json', store: load('leak-before') }, {})
    expect(report.failures).toEqual([])
    expect(report.findings.map((finding) => finding.kind)).toContain('accumulation-point')
    expect(report.classes[0]!.key).toBeDefined()
    expect(report.growth!.find((row) => row.key === 'Request')).toMatchObject({ countDelta: 150 })
  })

  it('fails on a finding of the given severity or worse', () => {
    const report = buildReport('leak-after.json', load('leak-after'), null, { failOn: 'high' })
    expect(report.failures.length).toBeGreaterThan(0)
    expect(report.failures[0]).toMatch(/^high finding: [a-z-]+$/)
    expect(report.failuresNamed.join(' ')).toContain('LeakyCache::requests')
    expect(report.failures.join(' ')).not.toContain('LeakyCache')
  })

  it('fails when a class grew by more than allowed', () => {
    const baseline = { name: 'leak-before.json', store: load('leak-before') }
    expect(buildReport('a', load('leak-after'), baseline, { maxGrowth: 149 }).failuresNamed).toContain('Request grew by 150 (more than 149)')
    expect(buildReport('a', load('leak-after'), baseline, { maxGrowth: 149 }).failures).toContain('a class grew by 150 (more than 149)')
    expect(buildReport('a', load('leak-after'), baseline, { maxGrowth: 1000 }).failures).toEqual([])
  })

  it('reads severities and renders an HTML page with names escaped', () => {
    expect(parseSeverity('medium')).toBe('medium')
    expect(parseSeverity('urgent')).toBeNull()
    const html = renderHtml(buildReport('<dump>.json', load('basic'), null, {}))
    expect(html).toContain('&lt;dump&gt;.json')
    expect(html).not.toContain('<script')
  })
})
