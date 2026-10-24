import { createLogger } from '../../log'

const log = createLogger('histogram')

/**
 * Class histograms over time: files written by the application from
 * meminfo_histogram(), either the array itself or wrapped with a timestamp:
 *
 *   file_put_contents("histo-" . time() . ".json",
 *       json_encode(['timestamp' => time(), 'classes' => meminfo_histogram()]));
 *
 * A few kilobytes each, so dozens can be compared: a class whose count grows
 * in every sample is the leak, without a single full dump (jmap -histo, taken
 * every few minutes).
 */
export interface HistogramPoint {
  name: string
  /** Unix seconds: the file's "timestamp", else its modification time. */
  time: number
  classes: Map<string, { count: number; size: number }>
}

export interface HistogramTrendRow {
  key: string
  counts: number[]
  sizes: number[]
  /** The count went up at every sample. */
  growing: boolean
}

export interface HistogramTrend {
  names: string[]
  times: number[]
  rows: HistogramTrendRow[]
}

export class InvalidHistogramError extends Error {}

export function parseHistogram(text: string, name: string, modifiedMs: number): HistogramPoint {
  let parsed: unknown
  try {
    parsed = JSON.parse(text)
  } catch (error) {
    throw new InvalidHistogramError(`${name} is not JSON: ${error instanceof Error ? error.message : String(error)}`)
  }
  if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
    throw new InvalidHistogramError(`${name} is not a histogram`)
  }
  const wrapper = parsed as { timestamp?: unknown; classes?: unknown }
  const source = wrapper.classes !== undefined ? wrapper.classes : parsed
  if (source === null || typeof source !== 'object' || Array.isArray(source)) {
    throw new InvalidHistogramError(`${name} has no classes`)
  }

  const classes = new Map<string, { count: number; size: number }>()
  for (const [key, row] of Object.entries(source as Record<string, unknown>)) {
    const { count, size } = (row ?? {}) as { count?: unknown; size?: unknown }
    if (typeof count !== 'number') {
      throw new InvalidHistogramError(`${name}: ${key} has no count; is it the output of meminfo_histogram()?`)
    }
    classes.set(key, { count, size: typeof size === 'number' ? size : 0 })
  }
  const time = typeof wrapper.timestamp === 'number' && wrapper.classes !== undefined ? wrapper.timestamp : Math.floor(modifiedMs / 1000)
  return { name, time, classes }
}

export function histogramTrend(points: HistogramPoint[]): HistogramTrend {
  const sorted = [...points].sort((a, b) => a.time - b.time)
  const keys = new Set<string>()
  for (const point of sorted) for (const key of point.classes.keys()) keys.add(key)

  const rows: HistogramTrendRow[] = []
  for (const key of keys) {
    const counts = sorted.map((point) => point.classes.get(key)?.count ?? 0)
    const sizes = sorted.map((point) => point.classes.get(key)?.size ?? 0)
    let growing = counts.length >= 2
    for (let at = 1; at < counts.length; at++) if (counts[at]! <= counts[at - 1]!) growing = false
    rows.push({ key, counts, sizes, growing })
  }
  const growth = (row: HistogramTrendRow): number => row.counts.at(-1)! - row.counts[0]!
  rows.sort((a, b) => Number(b.growing) - Number(a.growing) || growth(b) - growth(a) || a.key.localeCompare(b.key))
  log.info('histogram trend', { samples: sorted.length, classes: rows.length, growing: rows.filter((row) => row.growing).length })
  return { names: sorted.map((point) => point.name), times: sorted.map((point) => point.time), rows }
}
