/** Display helpers. Sizes come from the dump's estimates, so they are rounded, not exact. */

const UNITS = ['B', 'KB', 'MB', 'GB', 'TB']

export function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes)) return '—'
  let value = Math.abs(bytes)
  let unit = 0
  while (value >= 1024 && unit < UNITS.length - 1) {
    value /= 1024
    unit++
  }
  const text = unit === 0 ? String(Math.round(value)) : value.toFixed(value < 10 ? 1 : 0)
  return `${bytes < 0 ? '−' : ''}${text} ${UNITS[unit]}`
}

export function formatCount(value: number): string {
  return value.toLocaleString('en-US')
}

export function formatDelta(value: number, format: (v: number) => string = formatCount): string {
  if (value === 0) return '0'
  return value > 0 ? `+${format(value)}` : `−${format(-value)}`
}

export function formatPercent(part: number, whole: number): string {
  if (whole <= 0) return '—'
  const percent = (part / whole) * 100
  return percent < 0.1 && percent > 0 ? '<0.1%' : `${percent.toFixed(percent < 10 ? 1 : 0)}%`
}

export function formatDuration(ms: number): string {
  return ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(1)} s`
}

/** What an item is, in one short label: class for objects, else the type. */
export function describeItem(row: { type: string; className?: string | undefined; symbol?: string | undefined }): string {
  return row.className ?? row.type
}
