import { compareWithBaseline, reduceToBaseline, type GrowthRow } from '../engine/analyses/diff'
import { computeDominators } from '../engine/analyses/dominators'
import { findFindings, type Finding, type Severity } from '../engine/analyses/findings'
import { buildPredecessors } from '../engine/analyses/graph'
import { findSuspects } from '../engine/analyses/suspects'
import type { DumpHeader, DumpStore } from '../engine/store'

/**
 * The headless report: what the browser UI shows first, as data — the
 * findings, the classes retaining the most and, against a baseline, what
 * grew — plus the rules a CI job fails on (MAT's batch reports, memlab's CLI).
 * Pure: the command in report/cli.ts reads the files and writes the output.
 */
export interface Report {
  file: string
  header: DumpHeader
  items: number
  totalSize: number
  findings: Array<Omit<Finding, 'index'>>
  classes: Array<{ key: string; instances: number; retained: number }>
  growth: GrowthRow[] | null
  /** Rules that tripped, without names from the dump: safe to print in a CI log. */
  failures: string[]
  /** The same, with the titles of the findings and class names. */
  failuresNamed: string[]
}

export interface ReportRules {
  /** Fail on a finding of this severity or worse. */
  failOn?: Severity
  /** Fail when a class grew by more than this many instances since the baseline. */
  maxGrowth?: number
}

const RANK: Record<Severity, number> = { high: 0, medium: 1, low: 2 }

export function buildReport(file: string, store: DumpStore, baseline: { name: string; store: DumpStore } | null, rules: ReportRules): Report {
  const preds = buildPredecessors(store)
  const dominators = computeDominators(store, preds)
  const suspects = findSuspects(store, dominators, 50)
  const findings = findFindings(store, dominators, suspects, preds).map(({ index: _, ...finding }) => finding)
  const growth = baseline === null ? null : compareWithBaseline(reduceToBaseline(baseline.store, baseline.name), store).filter((row) => row.countDelta !== 0 || row.sizeDelta !== 0)

  const failures: string[] = []
  const failuresNamed: string[] = []
  if (rules.failOn !== undefined) {
    const limit = RANK[rules.failOn]
    for (const finding of findings) {
      if (RANK[finding.severity] > limit) continue
      failures.push(`${finding.severity} finding: ${finding.kind}`)
      failuresNamed.push(`${finding.severity} finding: ${finding.title}`)
    }
  }
  if (rules.maxGrowth !== undefined && growth !== null) {
    for (const row of growth) {
      if (row.countDelta <= rules.maxGrowth) continue
      failures.push(`a ${row.isClass ? 'class' : 'type'} grew by ${row.countDelta} (more than ${rules.maxGrowth})`)
      failuresNamed.push(`${row.key} grew by ${row.countDelta} (more than ${rules.maxGrowth})`)
    }
  }

  return {
    file,
    header: store.header,
    items: store.count,
    totalSize: suspects.totalSize,
    findings,
    classes: suspects.classes.slice(0, 20).map(({ key, instances, retained }) => ({ key, instances, retained })),
    growth,
    failures,
    failuresNamed,
  }
}

export function parseSeverity(value: string): Severity | null {
  return value === 'high' || value === 'medium' || value === 'low' ? value : null
}

function escape(text: string): string {
  return text.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]!)
}

function bytes(value: number): string {
  if (value >= 1 << 30) return `${(value / (1 << 30)).toFixed(1)} GB`
  if (value >= 1 << 20) return `${(value / (1 << 20)).toFixed(1)} MB`
  if (value >= 1 << 10) return `${(value / (1 << 10)).toFixed(1)} KB`
  return `${value} B`
}

/** A standalone HTML page: no script, no external resource. */
export function renderHtml(report: Report): string {
  const findings = report.findings
    .map((f) => `<li class="${f.severity}"><strong>${escape(f.severity)}</strong> ${escape(f.title)} <em>${bytes(f.bytes)}</em><p>${escape(f.hypothesis)}</p><p class="next">${escape(f.next)}</p></li>`)
    .join('\n')
  const classes = report.classes.map((c) => `<tr><td>${escape(c.key)}</td><td>${c.instances}</td><td>${bytes(c.retained)}</td></tr>`).join('\n')
  const growth =
    report.growth === null
      ? ''
      : `<h2>Growth since the baseline</h2><table><tr><th>Type or class</th><th>Before</th><th>Now</th><th>Δ</th></tr>${report.growth
          .slice(0, 30)
          .map((g) => `<tr><td>${escape(g.key)}</td><td>${g.before.count}</td><td>${g.after.count}</td><td>${g.countDelta > 0 ? '+' : ''}${g.countDelta}</td></tr>`)
          .join('\n')}</table>`
  const failures = report.failuresNamed.length === 0 ? '<p>No rule tripped.</p>' : `<ul class="failures">${report.failuresNamed.map((f) => `<li>${escape(f)}</li>`).join('')}</ul>`
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>meminfo report: ${escape(report.file)}</title>
<style>body{font:14px/1.5 system-ui,sans-serif;max-width:960px;margin:2rem auto;padding:0 1rem}li.high{border-left:3px solid #c22;padding-left:.5rem}li.medium{border-left:3px solid #c80;padding-left:.5rem}.next{color:#555}table{border-collapse:collapse}td,th{padding:2px 12px;border-bottom:1px solid #ddd;text-align:left}.failures{color:#c22}</style>
</head><body>
<h1>${escape(report.file)}</h1>
<p>${report.items} items, ${bytes(report.totalSize)} accounted for${typeof report.header.memory_usage === 'number' ? `, ${bytes(report.header.memory_usage)} in use` : ''}.</p>
<h2>Rules</h2>${failures}
<h2>Findings</h2><ul>${findings || '<li>Nothing stands out.</li>'}</ul>
<h2>Classes retaining the most</h2><table><tr><th>Class or type</th><th>Instances</th><th>Retained</th></tr>${classes}</table>
${growth}
</body></html>
`
}
