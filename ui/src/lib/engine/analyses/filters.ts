import { createLogger } from '../../log'
import { allocate } from '../columns'
import type { DumpStore } from '../store'

const log = createLogger('filters')

/**
 * Holders to ignore when computing what keeps items alive (Chrome's "Ignore
 * this retainer", memlab's retainerReferenceFilter, MAT's -excludes): a known
 * cache, the logger, framework internals. Ignoring them shows what else holds
 * an item, and retained sizes as if they did not hold anything.
 *
 *  - edge: the references named `name` from items of class or type `holder`;
 *  - holders: every reference from items whose class or type matches `pattern`.
 */
export type RetainerFilter = { kind: 'edge'; holder: string; name: string } | { kind: 'holders'; pattern: string }

export class InvalidFilterError extends Error {}

/** One flag per edge, 1 for an ignored edge; null when no filter applies. */
export function ignoredEdges(store: DumpStore, filters: RetainerFilter[]): Uint8Array | null {
  if (filters.length === 0) return null
  const started = performance.now()

  const patterns: RegExp[] = []
  const edges = new Map<string, Set<string>>()
  for (const filter of filters) {
    if (filter.kind === 'holders') {
      try {
        patterns.push(new RegExp(filter.pattern))
      } catch (error) {
        throw new InvalidFilterError(`invalid pattern ${filter.pattern}: ${error instanceof Error ? error.message : String(error)}`)
      }
      continue
    }
    const names = edges.get(filter.holder) ?? new Set<string>()
    names.add(filter.name)
    edges.set(filter.holder, names)
  }

  const ignored = allocate(Uint8Array, store.edgeCount, store.count)
  let count = 0
  for (let item = 0; item < store.count; item++) {
    const kind = store.className(item) ?? store.typeName(item)
    const all = patterns.some((pattern) => pattern.test(kind))
    const names = edges.get(kind)
    if (!all && names === undefined) continue
    for (let edge = store.childStart[item]!; edge < store.childStart[item + 1]!; edge++) {
      if (all || names!.has(store.edgeNameText(edge))) {
        ignored[edge] = 1
        count++
      }
    }
  }

  log.info('filters applied', { filters: filters.length, edges: count, ms: Math.round(performance.now() - started) })
  return ignored
}
