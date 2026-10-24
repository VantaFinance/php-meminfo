import { createLogger } from '../../log'
import { NONE, type DumpStore } from '../store'
import type { Predecessors } from './graph'
import { TopK } from './topk'

/**
 * The basic analyses, on the columnar store: summary, top children, query and
 * reference paths. They keep the semantics of the former PHP CLI analyzer
 * (SummaryCreator, TopSearcher, QueryExecutor, GraphBrowser), except where
 * noted; basic.test.ts checks them against baselines it recorded.
 */

const log = createLogger('analyses')

function timed<T>(name: string, run: () => T): T {
  const started = performance.now()
  const result = run()
  log.debug(name, { ms: Math.round(performance.now() - started) })
  return result
}

/* --- summary ------------------------------------------------------------- */

export interface SummaryRow {
  /** Type, or class for objects. */
  key: string
  isClass: boolean
  count: number
  selfSize: number
}

/**
 * Count and self size by type, objects grouped by class (SummaryCreator).
 * Sorted by count, then self size, then name — PHP leaves ties unordered.
 */
export function summarize(store: DumpStore): SummaryRow[] {
  return timed('summary', () => {
    const types = store.types.size
    const groups = types + store.classes.size
    const counts = new Float64Array(groups)
    const sizes = new Float64Array(groups)
    const objectType = store.types.find('object')

    for (let i = 0; i < store.count; i++) {
      const type = store.type[i]!
      const classId = store.classId[i]!
      const group = type === objectType && classId !== NONE ? types + classId : type
      counts[group]!++
      sizes[group]! += store.size[i]!
    }

    const rows: SummaryRow[] = []
    for (let group = 0; group < groups; group++) {
      if (counts[group] === 0) continue
      const isClass = group >= types
      rows.push({
        key: isClass ? store.classes.get(group - types) : store.types.get(group),
        isClass,
        count: counts[group]!,
        selfSize: sizes[group]!,
      })
    }
    return rows.sort((a, b) => b.count - a.count || b.selfSize - a.selfSize || a.key.localeCompare(b.key))
  })
}

/* --- top children -------------------------------------------------------- */

export interface TopChildrenRow {
  index: number
  /** Distinct child names — what PHP counts, as json_decode merges duplicates. */
  uniqueKeys: number
  /** Every edge, duplicates included. */
  edges: number
}

/** Items with the most children, among those with a `children` key (TopSearcher). */
export function topChildren(store: DumpStore, limit: number): TopChildrenRow[] {
  return timed('top children', () => {
    const top = new TopK(limit)
    const seen = new Set<number>()

    for (let i = 0; i < store.count; i++) {
      if (!store.hasChildrenKey(i)) continue
      const start = store.childStart[i]!
      const end = store.childStart[i + 1]!
      let count = end - start
      if (count > 1) {
        seen.clear()
        for (let edge = start; edge < end; edge++) seen.add(store.edgeName[edge]!)
        count = seen.size
      }
      top.push(count, i)
    }

    return top.result().map(({ score, index }) => ({
      index,
      uniqueKeys: score,
      edges: store.childStart[index + 1]! - store.childStart[index]!,
    }))
  })
}

/* --- query --------------------------------------------------------------- */

export type QueryAttribute = 'id' | 'type' | 'class' | 'symbol_name' | 'frame' | 'is_root' | 'size' | 'object_handle'

export interface QueryFilter {
  attribute: QueryAttribute
  operator: '=' | '~'
  value: string
}

export class InvalidQueryError extends Error {
  constructor(message: string) {
    super(message)
    this.name = 'InvalidQueryError'
  }
}

type Matcher = (store: DumpStore, index: number) => boolean

/** Text of an attribute, or undefined when the item does not have it. */
function attributeText(store: DumpStore, index: number, attribute: QueryAttribute): string | undefined {
  switch (attribute) {
    case 'id':
      return store.address(index)
    case 'type':
      return store.typeName(index)
    case 'class':
      return store.className(index)
    case 'symbol_name':
      return store.symbolId[index] === NONE ? undefined : store.symbols.get(store.symbolId[index]!)
    case 'frame':
      return store.frameId[index] === NONE ? undefined : store.frames.get(store.frameId[index]!)
    case 'is_root':
      return store.isRoot(index) ? '1' : '0'
    case 'size':
      return String(store.size[index]!)
    case 'object_handle':
      return store.handle[index] === NONE ? undefined : String(store.handle[index]!)
  }
}

function parseBoolean(value: string): boolean {
  const normalized = value.trim().toLowerCase()
  if (['1', 'true', 'yes'].includes(normalized)) return true
  if (['0', 'false', 'no', ''].includes(normalized)) return false
  throw new InvalidQueryError(`is_root expects true or false, got "${value}"`)
}

function compile(filter: QueryFilter): Matcher {
  const { attribute, operator, value } = filter

  if (operator === '~') {
    let pattern: RegExp
    try {
      pattern = new RegExp(value)
    } catch {
      throw new InvalidQueryError(`invalid regular expression: ${value}`)
    }
    return (store, index) => {
      const text = attributeText(store, index, attribute)
      return text !== undefined && pattern.test(text)
    }
  }

  // '=' is PHP's loose ==, except that is_root takes a real boolean: PHP's
  // `is_root=false` matches roots, because true == "false".
  if (attribute === 'is_root') {
    const wanted = parseBoolean(value)
    return (store, index) => store.isRoot(index) === wanted
  }
  if (attribute === 'size' || attribute === 'object_handle') {
    const wanted = Number(value)
    if (Number.isNaN(wanted)) {
      throw new InvalidQueryError(`${attribute} expects a number, got "${value}"`)
    }
    return (store, index) => {
      const text = attributeText(store, index, attribute)
      return text !== undefined && Number(text) === wanted
    }
  }
  return (store, index) => attributeText(store, index, attribute) === value
}

export interface QueryResult {
  indices: number[]
  /** Matches in the whole dump. */
  total: number
}

/**
 * Items matching every filter, in dump order (QueryExecutor). `offset`/`limit`
 * page through them; `total` counts all matches.
 */
export function query(store: DumpStore, filters: QueryFilter[], offset = 0, limit = 10): QueryResult {
  return timed('query', () => {
    const matchers = filters.map(compile)
    const indices: number[] = []
    let total = 0

    for (let i = 0; i < store.count; i++) {
      let matches = true
      for (const matcher of matchers) {
        if (!matcher(store, i)) {
          matches = false
          break
        }
      }
      if (!matches) continue
      if (total >= offset && indices.length < limit) indices.push(i)
      total++
    }
    return { indices, total }
  })
}

/* --- referrers and reference paths -------------------------------------- */

export interface Referrer {
  source: number
  edge: number
}

export function referrers(preds: Predecessors, index: number): Referrer[] {
  const out: Referrer[] = []
  for (let at = preds.predStart[index]!; at < preds.predStart[index + 1]!; at++) {
    out.push({ source: preds.predSource[at]!, edge: preds.predEdge[at]! })
  }
  return out
}

/** One way down from a root to the target: `steps[0].item` is the root. */
export interface ReferencePath {
  root: number
  /** The target is the root itself (GraphBrowser's `<self>`). */
  self: boolean
  /** Each step enters `item` through `edge` from the previous step (edge is -1 on the root). */
  steps: Array<{ item: number; edge: number }>
}

/**
 * Shortest path from every root that reaches `target` (GraphBrowser): a BFS
 * from the target over incoming edges. Scratch arrays are reused between calls,
 * with a generation stamp instead of clearing them.
 */
export class ReferencePathFinder {
  private readonly store: DumpStore
  private readonly preds: Predecessors
  private readonly stamp: Uint32Array
  private readonly via: Int32Array
  private readonly viaEdge: Int32Array
  private readonly queue: Int32Array
  private readonly ignored: Uint8Array | null
  private generation = 0

  constructor(store: DumpStore, preds: Predecessors, ignored: Uint8Array | null = null) {
    this.store = store
    this.preds = preds
    this.ignored = ignored
    this.stamp = new Uint32Array(store.count)
    this.via = new Int32Array(store.count)
    this.viaEdge = new Int32Array(store.count)
    this.queue = new Int32Array(store.count)
  }

  find(target: number, maxPaths = 50): { paths: ReferencePath[]; rootsReaching: number } {
    return timed('reference paths', () => {
      const { predStart, predSource, predEdge } = this.preds
      const generation = ++this.generation
      this.stamp[target] = generation
      this.via[target] = -1
      this.viaEdge[target] = -1

      const roots: number[] = []
      let head = 0
      let tail = 0
      this.queue[tail++] = target

      while (head < tail) {
        const item = this.queue[head++]!
        if (item !== target && this.store.isRoot(item)) roots.push(item)
        for (let at = predStart[item]!; at < predStart[item + 1]!; at++) {
          const parent = predSource[at]!
          if (this.stamp[parent] === generation) continue
          if (this.ignored !== null && this.ignored[predEdge[at]!] === 1) continue
          this.stamp[parent] = generation
          this.via[parent] = item
          this.viaEdge[parent] = predEdge[at]!
          this.queue[tail++] = parent
        }
      }

      const paths: ReferencePath[] = []
      if (this.store.isRoot(target)) {
        paths.push({ root: target, self: true, steps: [{ item: target, edge: -1 }] })
      }
      // BFS order is already by distance from the target.
      for (const root of roots) {
        if (paths.length >= maxPaths) break
        const steps: ReferencePath['steps'] = [{ item: root, edge: -1 }]
        let item = root
        while (item !== target) {
          const edge = this.viaEdge[item]!
          item = this.via[item]!
          steps.push({ item, edge })
        }
        paths.push({ root, self: false, steps })
      }
      return { paths, rootsReaching: roots.length + (this.store.isRoot(target) ? 1 : 0) }
    })
  }
}
