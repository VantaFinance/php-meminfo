import { createLogger } from '../../log'
import { allocate } from '../columns'
import { unattributedMemory, type DumpStore } from '../store'
import type { Dominators } from './dominators'
import type { Predecessors } from './graph'
import type { SuspectReport } from './suspects'

const log = createLogger('findings')

/**
 * Automatic findings: patterns of a dump that usually mean a leak or waste,
 * each with a one-line hypothesis and what to look at next — the leak suspects
 * report of Eclipse MAT and the inspections of dotMemory, for PHP.
 *
 *  - dominant class: one class retains a large share of the dump;
 *  - accumulation point: one holder keeps many small items alive (a cache, a
 *    registry), none of them big on its own;
 *  - cycle: strongly connected items that live and die together;
 *  - closures: many closures alive from one declaration (listeners registered
 *    per request and never removed);
 *  - sparse arrays: arrays whose allocated slots are mostly empty;
 *  - highly referenced: one item held from an unusual number of places;
 *  - unattributed: most of PHP's memory is held by no item at all;
 *  - outside the Zend allocator: the process is much bigger than what PHP
 *    allocated (extensions and libraries using malloc).
 *
 * Thresholds are shares of the dump, so small dumps produce no noise.
 */

export type FindingKind =
  | 'dominant-class'
  | 'accumulation-point'
  | 'cycle'
  | 'closures'
  | 'sparse-arrays'
  | 'highly-referenced'
  | 'unattributed'
  | 'outside-zend'
  | 'unreachable'
  | 'duplicate-strings'

export type Severity = 'high' | 'medium' | 'low'

export interface Finding {
  kind: FindingKind
  severity: Severity
  title: string
  hypothesis: string
  next: string
  /** Bytes the finding is about: retained, wasted or unattributed. */
  bytes: number
  /** Items concerned (instances, members, closures, arrays, referrers). */
  count: number
  /** An item to open, -1 when the finding is about a class or the whole dump. */
  index: number
  /** For class-level findings. */
  className?: string
}

/** A class retaining at least this share of the dump is reported. */
const DOMINANT_CLASS_SHARE = 0.3
/** An accumulation point retains at least this share of the dump… */
const ACCUMULATION_SHARE = 0.05
/** …through at least this many directly dominated items… */
const ACCUMULATION_MIN_CHILDREN = 100
/** …none of which retains more than this share of it. */
const ACCUMULATION_MAX_CHILD = 0.2
const CYCLE_SHARE = 0.01
const CLOSURES_MIN = 20
const SPARSE_MIN_CAPACITY = 64
/** An array is sparse when at most this share of its slots holds a value. */
const SPARSE_MAX_FILL = 0.25
const SPARSE_MIN_WASTE = 1 << 20
const HIGHLY_REFERENCED_MIN = 10_000
const UNATTRIBUTED_SHARE = 0.5
const UNATTRIBUTED_MIN = 16 << 20
const PER_KIND = 5

function severityOf(share: number): Severity {
  if (share >= 0.25) return 'high'
  if (share >= 0.05) return 'medium'
  return 'low'
}

export function findFindings(store: DumpStore, dominators: Dominators, suspects: SuspectReport, preds: Predecessors): Finding[] {
  const started = performance.now()
  const total = suspects.totalSize
  const findings: Finding[] = []
  if (total <= 0) return findings

  const counts: Record<string, number> = {}
  const add = (kind: FindingKind, list: Finding[]): void => {
    counts[kind] = list.length
    findings.push(...list)
  }

  add('dominant-class', dominantClasses(suspects, total))
  add('accumulation-point', accumulationPoints(store, dominators, total))
  add('cycle', cycles(store, dominators, total))
  add('closures', closureGroups(store, dominators, total))
  add('sparse-arrays', sparseArrays(store, total))
  add('highly-referenced', highlyReferenced(store, preds))
  add('unattributed', unattributed(store, total))
  add('outside-zend', outsideZend(store))
  add('unreachable', unreachableObjects(store, dominators, total))
  add('duplicate-strings', duplicateStrings(store, preds, total))

  const rank: Record<Severity, number> = { high: 0, medium: 1, low: 2 }
  findings.sort((a, b) => rank[a.severity] - rank[b.severity] || b.bytes - a.bytes)
  log.info('findings', { ...counts, ms: Math.round(performance.now() - started) })
  return findings
}

/** "Foo $bar" for an item: its class or type, and its symbol when it is a root. */
function labelOf(store: DumpStore, index: number): string {
  const kind = store.className(index) ?? store.typeName(index)
  const symbol = store.symbolId[index]! >= 0 ? store.symbols.get(store.symbolId[index]!) : undefined
  return symbol === undefined ? kind : `${kind} ${symbol}`
}

function percent(part: number, whole: number): string {
  return `${Math.round((part / whole) * 100)}%`
}

function dominantClasses(suspects: SuspectReport, total: number): Finding[] {
  return suspects.classes
    .filter((row) => row.isClass && row.retained >= total * DOMINANT_CLASS_SHARE)
    .slice(0, PER_KIND)
    .map((row) => ({
      kind: 'dominant-class' as const,
      severity: severityOf(row.retained / total),
      title: `${row.key} retains ${percent(row.retained, total)} of the dump`,
      hypothesis: `${row.instances} instances, ${row.topMost} of them not held by another ${row.key}: either the process really works on that many, or they are kept after use.`,
      next: 'List the instances (Search) and compare their count with what one request or job needs; check the paths of a few.',
      bytes: row.retained,
      count: row.instances,
      index: -1,
      className: row.key,
    }))
}

function accumulationPoints(store: DumpStore, dominators: Dominators, total: number): Finding[] {
  const n = store.count
  const { idom, retained, virtualRoot } = dominators
  const children = allocate(Uint32Array, n, n)
  const biggestChild = allocate(Float64Array, n, n)
  for (let i = 0; i < n; i++) {
    const dominator = idom[i]!
    if (dominator < 0 || dominator === virtualRoot) continue
    children[dominator]!++
    if (retained[i]! > biggestChild[dominator]!) biggestChild[dominator] = retained[i]!
  }

  const found: Finding[] = []
  for (let i = 0; i < n; i++) {
    const own = retained[i]!
    if (own < total * ACCUMULATION_SHARE || children[i]! < ACCUMULATION_MIN_CHILDREN) continue
    if (biggestChild[i]! >= own * ACCUMULATION_MAX_CHILD) continue
    found.push({
      kind: 'accumulation-point',
      severity: severityOf(own / total),
      title: `${labelOf(store, i)} keeps ${children[i]!} items alive, none of them big`,
      hypothesis: 'One holder accumulates many small items: a cache, a registry or a collection that only grows.',
      next: 'Open it and find what adds to it; look for a missing removal or a size limit.',
      bytes: own,
      count: children[i]!,
      index: i,
    })
  }
  return found.sort((a, b) => b.bytes - a.bytes).slice(0, PER_KIND)
}

/**
 * Strongly connected components of two items or more (iterative Tarjan), with
 * the retained size of the members the cycle itself does not dominate from
 * inside — what the cycle as a whole keeps alive.
 */
function cycles(store: DumpStore, dominators: Dominators, total: number): Finding[] {
  const n = store.count
  const { childStart, edgeTarget } = store
  const { idom, retained } = dominators
  const order = allocate(Int32Array, n, n).fill(-1)
  const low = allocate(Int32Array, n, n)
  const component = allocate(Int32Array, n, n).fill(-1)
  const onStack = allocate(Uint8Array, n, n)
  const stack = allocate(Int32Array, n, n)
  const callNode = allocate(Int32Array, n, n)
  const callEdge = allocate(Uint32Array, n, n)
  let counter = 0
  let top = 0
  let components = 0
  const sizes: number[] = []

  for (let start = 0; start < n; start++) {
    if (order[start] !== -1) continue
    let depth = 0
    callNode[0] = start
    callEdge[0] = childStart[start]!
    order[start] = low[start] = counter++
    stack[top++] = start
    onStack[start] = 1

    while (depth >= 0) {
      const node = callNode[depth]!
      const edge = callEdge[depth]!
      if (edge < childStart[node + 1]!) {
        callEdge[depth] = edge + 1
        const next = edgeTarget[edge]!
        if (next < 0) continue
        if (order[next] === -1) {
          order[next] = low[next] = counter++
          stack[top++] = next
          onStack[next] = 1
          depth++
          callNode[depth] = next
          callEdge[depth] = childStart[next]!
        } else if (onStack[next] === 1 && order[next]! < low[node]!) {
          low[node] = order[next]!
        }
        continue
      }

      if (low[node] === order[node]) {
        let size = 0
        let member: number
        do {
          member = stack[--top]!
          onStack[member] = 0
          component[member] = components
          size++
        } while (member !== node)
        sizes.push(size)
        components++
      }
      depth--
      if (depth >= 0) {
        const parent = callNode[depth]!
        if (low[node]! < low[parent]!) low[parent] = low[node]!
      }
    }
  }

  const cycleRetained = new Map<number, { bytes: number; representative: number }>()
  for (let i = 0; i < n; i++) {
    const c = component[i]!
    if (sizes[c]! < 2 || idom[i]! < 0) continue
    const dominator = idom[i]!
    if (dominator < n && component[dominator] === c) continue
    const entry = cycleRetained.get(c) ?? { bytes: 0, representative: i }
    entry.bytes += retained[i]!
    if (retained[i]! > retained[entry.representative]!) entry.representative = i
    cycleRetained.set(c, entry)
  }

  const found: Finding[] = []
  for (const [c, { bytes, representative }] of cycleRetained) {
    if (bytes < total * CYCLE_SHARE) continue
    found.push({
      kind: 'cycle',
      severity: severityOf(bytes / total),
      title: `A cycle of ${sizes[c]!} items keeps ${percent(bytes, total)} of the dump alive`,
      hypothesis: 'Items that reference each other live and die together: one reference from a root into the cycle keeps all of it, and PHP frees a cycle only in the cycle collector.',
      next: 'Show the graph of one member and find the edge that leads into the cycle from a root; break it, or the back reference.',
      bytes,
      count: sizes[c]!,
      index: representative,
    })
  }
  return found.sort((a, b) => b.bytes - a.bytes).slice(0, PER_KIND)
}

function closureGroups(store: DumpStore, dominators: Dominators, total: number): Finding[] {
  const groups = new Map<string, { count: number; bytes: number; representative: number }>()
  for (const [index, extras] of store.extras) {
    const where = extras.declaredAt ?? extras.function
    if (where === undefined || store.className(index) !== 'Closure' || dominators.idom[index]! < 0) continue
    const entry = groups.get(where) ?? { count: 0, bytes: 0, representative: index }
    entry.count++
    entry.bytes += dominators.retained[index]!
    if (dominators.retained[index]! > dominators.retained[entry.representative]!) entry.representative = index
    groups.set(where, entry)
  }

  const found: Finding[] = []
  for (const [where, { count, bytes, representative }] of groups) {
    if (count < CLOSURES_MIN) continue
    found.push({
      kind: 'closures',
      severity: severityOf(Math.max(bytes / total, count >= 1000 ? 0.05 : 0)),
      title: `${count} closures from ${where} are alive`,
      hypothesis: 'Many closures from one place at once: listeners or callbacks registered on every request or job and never removed.',
      next: 'Open one and follow its reference paths to where it is registered; unregister it when the work is done.',
      bytes,
      count,
      index: representative,
    })
  }
  return found.sort((a, b) => b.count - a.count).slice(0, PER_KIND)
}

function sparseArrays(store: DumpStore, total: number): Finding[] {
  let wasted = 0
  let arrays = 0
  let worst = -1
  let worstWaste = 0
  for (const [index, extras] of store.extras) {
    const capacity = extras.capacity
    if (capacity === undefined || capacity < SPARSE_MIN_CAPACITY) continue
    const live = store.childStart[index + 1]! - store.childStart[index]!
    if (live > capacity * SPARSE_MAX_FILL) continue
    // Bytes per slot from the array's own size: 16 to 40 by layout and version.
    const perSlot = Math.max(16, (store.size[index]! - 56) / capacity)
    const waste = (capacity - live) * perSlot
    wasted += waste
    arrays++
    if (waste > worstWaste) {
      worstWaste = waste
      worst = index
    }
  }
  if (arrays === 0 || wasted < Math.max(SPARSE_MIN_WASTE, total * 0.01)) return []
  return [
    {
      kind: 'sparse-arrays',
      severity: severityOf(wasted / total),
      title: `${arrays} arrays are mostly empty slots, ${percent(wasted, total)} of the dump`,
      hypothesis: 'An array keeps its allocated slots after elements are removed: a queue or buffer that once grew large still holds that memory.',
      next: 'Open the worst one; copy it into a new array after a bulk removal, or recreate it instead of unsetting elements.',
      bytes: wasted,
      count: arrays,
      index: worst,
    },
  ]
}

function highlyReferenced(store: DumpStore, preds: Predecessors): Finding[] {
  const found: Finding[] = []
  for (let i = 0; i < store.count; i++) {
    const referrers = preds.predStart[i + 1]! - preds.predStart[i]!
    if (referrers < HIGHLY_REFERENCED_MIN) continue
    found.push({
      kind: 'highly-referenced',
      severity: 'low',
      title: `${labelOf(store, i)} is referenced from ${referrers} places`,
      hypothesis: 'Shared by an unusual number of holders: fine for a service, but whatever it holds lives as long as the last of them.',
      next: 'Check that sharing it is intended, and what it holds (children, retained size).',
      bytes: store.size[i]!,
      count: referrers,
      index: i,
    })
  }
  return found.sort((a, b) => b.count - a.count).slice(0, PER_KIND)
}

function unattributed(store: DumpStore, total: number): Finding[] {
  const memory = store.header.memory_usage
  const held = unattributedMemory(store.header, total)
  if (held === null || memory === undefined || held < UNATTRIBUTED_MIN || held < memory * UNATTRIBUTED_SHARE) return []
  return [
    {
      kind: 'unattributed',
      severity: 'medium',
      title: `${percent(held, memory)} of PHP's memory is held by no item`,
      hypothesis: 'The memory is in extension internals (mysqlnd buffered results, DOM documents, image libraries), compiled code or allocator overhead, not in PHP values.',
      next: 'Look for unbuffered queries, freed documents and images; compare user_class_count between dumps for code generated at runtime.',
      bytes: held,
      count: 0,
      index: -1,
    },
  ]
}

/** RSS at least this many times what the Zend allocator holds… */
const OUTSIDE_ZEND_RATIO = 2
/** …and at least this much more. */
const OUTSIDE_ZEND_MIN = 64 << 20

function outsideZend(store: DumpStore): Finding[] {
  const rss = store.header.rss
  const zend = store.header.memory_usage_real ?? store.header.memory_usage
  if (rss === undefined || zend === undefined || rss < zend * OUTSIDE_ZEND_RATIO || rss - zend < OUTSIDE_ZEND_MIN) return []
  const outside = rss - zend
  return [
    {
      kind: 'outside-zend',
      severity: 'medium',
      title: `The process is ${percent(rss, zend)} the size of what PHP allocated`,
      hypothesis: `${Math.round(outside / (1 << 20))} MB are resident outside the Zend allocator: extensions and libraries that use malloc (Imagick, GD, librdkafka, gRPC, mysqlnd with persistent buffers), the code itself, or memory freed by PHP and not returned to the system.`,
      next: 'No PHP value holds it, so this dump cannot show it: compare RSS between dumps, and profile the native side (heaptrack, jemalloc profiling) if it keeps growing.',
      bytes: outside,
      count: 0,
      index: -1,
    },
  ]
}

/**
 * Objects the extension found alive but reached from no root (dumped with the
 * "unreachable" option, frame "<UNREACHABLE>"): cycles waiting for the cycle
 * collector, or objects held from C code.
 */
function unreachableObjects(store: DumpStore, dominators: Dominators, total: number): Finding[] {
  const frame = store.frames.find('<UNREACHABLE>')
  if (frame < 0) return []
  let roots = 0
  let bytes = 0
  let biggest = -1
  for (let i = 0; i < store.count; i++) {
    if (store.frameId[i] !== frame || !store.isRoot(i)) continue
    roots++
    bytes += dominators.retained[i]!
    if (biggest < 0 || dominators.retained[i]! > dominators.retained[biggest]!) biggest = i
  }
  if (roots === 0) return []
  return [
    {
      kind: 'unreachable',
      severity: severityOf(bytes / total),
      title: `${roots} objects are alive with nothing pointing at them, ${percent(bytes, total)} of the dump`,
      hypothesis: 'They reference each other in cycles the cycle collector has not freed yet (gc disabled, or its threshold not reached), or an extension holds them from C code.',
      next: 'Run gc_collect_cycles() and dump again: what remains is held from C. Break the cycles (unset the back references) where they are made.',
      bytes,
      count: roots,
      index: biggest,
    },
  ]
}

const DUPLICATES_MIN_WASTE = 1 << 20

/**
 * Strings held several times over (dumped with the "string_hashes" option):
 * every copy beyond the first is waste — MAT's and dotMemory's duplicate
 * strings. Reported with the class that holds the most copies.
 */
function duplicateStrings(store: DumpStore, preds: Predecessors, total: number): Finding[] {
  const groups = new Map<string, number[]>()
  for (const [index, extras] of store.extras) {
    if (extras.hash === undefined) continue
    const group = groups.get(extras.hash)
    if (group === undefined) groups.set(extras.hash, [index])
    else group.push(index)
  }

  let wasted = 0
  let copies = 0
  let worst: number[] = []
  let worstWaste = 0
  for (const members of groups.values()) {
    if (members.length < 2) continue
    const waste = (members.length - 1) * store.size[members[0]!]!
    wasted += waste
    copies += members.length - 1
    if (waste > worstWaste) {
      worstWaste = waste
      worst = members
    }
  }
  if (copies === 0 || wasted < Math.max(DUPLICATES_MIN_WASTE, total * 0.01)) return []

  // The holder class of the worst group's copies, from their first referrer.
  const holders = new Map<string, number>()
  for (const index of worst) {
    const start = preds.predStart[index]!
    if (start === preds.predStart[index + 1]!) continue
    const holder = labelOf(store, preds.predSource[start]!).split(' ')[0]!
    holders.set(holder, (holders.get(holder) ?? 0) + 1)
  }
  const holder = [...holders.entries()].sort((a, b) => b[1] - a[1])[0]?.[0] ?? 'unknown holders'

  return [
    {
      kind: 'duplicate-strings',
      severity: severityOf(wasted / total),
      title: `${copies} duplicate strings waste ${percent(wasted, total)} of the dump`,
      hypothesis: `The same text is held as separate strings; the biggest group has ${worst.length} copies, mostly held by ${holder}. Strings built at runtime (concatenation, json_decode, database rows) are not shared even when equal.`,
      next: 'Open one copy and find where they are built; reuse one value (a map of interned values, enum or constant) instead of rebuilding it.',
      bytes: wasted,
      count: copies,
      index: worst[0] ?? -1,
    },
  ]
}

