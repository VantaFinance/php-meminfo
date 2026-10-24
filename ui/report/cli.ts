/**
 * meminfo-report: the browser UI's analysis without a browser, for CI.
 *
 *   node dist-report/cli.js <dump.json> [--baseline=<earlier.json>] [--format=json|html]
 *       [--fail-on=high|medium|low] [--max-growth=N] [--show-names] [--verbose]
 *
 * Writes the report to stdout; exits 1 when a rule trips, 2 on a usage error or
 * a dump that cannot be read. Diagnostics go to stderr and contain no class,
 * symbol or key from the dump unless --show-names is given: CI logs are often
 * more widely readable than the report itself. Build it with
 * `npm run build:report`.
 */
import { createReadStream, statSync } from 'node:fs'
import { basename } from 'node:path'
import { Readable } from 'node:stream'
import { parseDumpStream } from '../src/lib/engine/parser'
import type { DumpStore } from '../src/lib/engine/store'
import { setLogLevel } from '../src/lib/log'
import { buildReport, parseSeverity, renderHtml, type ReportRules } from '../src/lib/report/report'

const USAGE =
  'usage: meminfo-report <dump.json> [--baseline=<earlier.json>] [--format=json|html] [--fail-on=high|medium|low] [--max-growth=N] [--show-names] [--verbose]'

function fail(message: string): never {
  process.stderr.write(`error: ${message}\n${USAGE}\n`)
  process.exit(2)
}

async function load(path: string, verbose: boolean): Promise<DumpStore> {
  const started = performance.now()
  const stream = Readable.toWeb(createReadStream(path)) as ReadableStream<Uint8Array>
  const store = await parseDumpStream(stream, { expectedBytes: statSync(path).size })
  if (verbose) process.stderr.write(`meminfo-report: read ${basename(path)}: ${store.count} items in ${Math.round(performance.now() - started)} ms\n`)
  return store
}

async function main(argv: string[]): Promise<number> {
  let dump: string | null = null
  let baseline: string | null = null
  let format = 'json'
  let verbose = false
  let showNames = false
  const rules: ReportRules = {}

  for (const argument of argv) {
    const [name, value] = argument.includes('=') ? [argument.slice(0, argument.indexOf('=')), argument.slice(argument.indexOf('=') + 1)] : [argument, '']
    switch (name) {
      case '--baseline':
        baseline = value
        break
      case '--format':
        if (value !== 'json' && value !== 'html') fail('--format must be json or html')
        format = value
        break
      case '--fail-on': {
        const severity = parseSeverity(value)
        if (severity === null) fail('--fail-on must be high, medium or low')
        rules.failOn = severity
        break
      }
      case '--max-growth':
        if (!/^\d+$/.test(value)) fail('--max-growth must be a number of instances')
        rules.maxGrowth = Number(value)
        break
      case '--verbose':
        verbose = true
        break
      case '--show-names':
        showNames = true
        break
      case '-h':
      case '--help':
        process.stdout.write(`${USAGE}\n`)
        return 0
      default:
        if (name.startsWith('-')) fail(`unknown option ${name}`)
        if (dump !== null) fail('only one dump may be given')
        dump = argument
    }
  }
  if (dump === null) fail('no dump given')

  // The engine logs through console; stdout carries the report alone.
  const toStderr = (...args: unknown[]): void => {
    process.stderr.write(`${args.map((arg) => (typeof arg === 'string' ? arg : JSON.stringify(arg))).join(' ')}\n`)
  }
  console.log = console.info = console.debug = console.warn = toStderr
  setLogLevel(verbose ? 'info' : 'warn')
  const started = performance.now()
  const store = await load(dump, verbose)
  const earlier = baseline === null ? null : { name: basename(baseline), store: await load(baseline, verbose) }
  const report = buildReport(basename(dump), store, earlier, rules)
  if (verbose) process.stderr.write(`meminfo-report: analysed in ${Math.round(performance.now() - started)} ms, ${report.findings.length} findings\n`)

  process.stdout.write(format === 'html' ? renderHtml(report) : `${JSON.stringify(report, null, 2)}\n`)
  for (const failure of showNames ? report.failuresNamed : report.failures) process.stderr.write(`meminfo-report: ${failure}\n`)
  return report.failures.length === 0 ? 0 : 1
}

main(process.argv.slice(2)).then(
  (code) => process.exit(code),
  (error: unknown) => {
    process.stderr.write(`error: ${error instanceof Error ? error.message : String(error)}\n`)
    process.exit(2)
  },
)
