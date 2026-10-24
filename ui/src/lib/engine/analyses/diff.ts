import { createLogger } from '../../log'
import { NONE, type DumpHeader, type DumpStore } from '../store'
import { summarize } from './basic'
import { rootDistances } from './graph'
import { pathSignature } from './retainers'

const log = createLogger('diff')

/**
 * Growth between two dumps of the same process — the documented way to find a
 * leak: what keeps growing from one dump to the next.
 *
 * Only aggregates of the baseline are kept, so loading it costs a parse and a
 * few kilobytes, not a second store: counts per class, the biggest containers
 * by the shape of their root path, and each object's (handle, class) pair in
 * two typed arrays — 8 bytes an object. Addresses are never compared across
 * dumps: they mean nothing outside the dump they come from. A handle names an
 * object for its whole life, though a freed handle is reused; paired with the
 * class it is a fair identity, the one three-dump detection relies on.
 * Retained sizes are not compared — under sharing they move around without
 * anything growing.
 */

export interface BaselineRow {
  key: string
  isClass: boolean
  count: number
  selfSize: number
}

export interface BaselineObjects {
  handles: Int32Array
  /** Index into classNames, per object. */
  classes: Int32Array
  classNames: string[]
}

export interface BaselineContainer {
  /** Root path shape, see retainers.pathSignature(). */
  signature: string
  children: number
}

export interface Baseline {
  name: string
  items: number
  totalSize: number
  rows: BaselineRow[]
  /** Absent in baselines made before series and three-dump detection. */
  header?: DumpHeader
  objects?: BaselineObjects
  containers?: BaselineContainer[]
}

/** Containers with at least this many children are followed from dump to dump… */
const CONTAINER_MIN_CHILDREN = 100
/** …up to this many, the biggest first. */
const CONTAINERS_KEPT = 2000

export function reduceToBaseline(store: DumpStore, name: string): Baseline {
  const rows = summarize(store)
  const baseline: Baseline = {
    name,
    items: store.count,
    totalSize: rows.reduce((sum, row) => sum + row.selfSize, 0),
    rows: rows.map(({ key, isClass, count, selfSize }) => ({ key, isClass, count, selfSize })),
    header: store.header,
    objects: objectIdentities(store),
    containers: containers(store),
  }
  log.info('baseline reduced', {
    items: baseline.items,
    rows: baseline.rows.length,
    objects: baseline.objects!.handles.length,
    containers: baseline.containers!.length,
  })
  return baseline
}

/** (handle, class) of every object of the dump. */
export function objectIdentities(store: DumpStore): BaselineObjects {
  let count = 0
  for (let i = 0; i < store.count; i++) if (store.handle[i] !== NONE && store.classId[i] !== NONE) count++
  const handles = new Int32Array(count)
  const classes = new Int32Array(count)
  let at = 0
  for (let i = 0; i < store.count; i++) {
    if (store.handle[i] === NONE || store.classId[i] === NONE) continue
    handles[at] = store.handle[i]!
    classes[at] = store.classId[i]!
    at++
  }
  const classNames: string[] = []
  for (let id = 0; id < store.classes.size; id++) classNames.push(store.classes.get(id))
  return { handles, classes, classNames }
}

/** The biggest containers, keyed by the shape of their shortest root path. */
export function containers(store: DumpStore): BaselineContainer[] {
  const candidates: number[] = []
  for (let i = 0; i < store.count; i++) {
    if (store.childStart[i + 1]! - store.childStart[i]! >= CONTAINER_MIN_CHILDREN) candidates.push(i)
  }
  if (candidates.length === 0) return []
  candidates.sort((a, b) => store.childStart[b + 1]! - store.childStart[b]! - (store.childStart[a + 1]! - store.childStart[a]!))
  const distances = rootDistances(store)
  const bySignature = new Map<string, number>()
  for (const index of candidates.slice(0, CONTAINERS_KEPT)) {
    const signature = pathSignature(store, distances, index).join(' ')
    bySignature.set(signature, (bySignature.get(signature) ?? 0) + store.childStart[index + 1]! - store.childStart[index]!)
  }
  return [...bySignature.entries()].map(([signature, children]) => ({ signature, children }))
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

export interface SeriesRow {
  key: string
  isClass: boolean
  /** One count per dump, oldest first, the current dump last. */
  counts: number[]
  sizes: number[]
}

export interface SeriesResult {
  /** Dump names, oldest first. */
  names: string[]
  /** Types and classes whose count grew at every step. */
  classes: SeriesRow[]
  /** Containers whose children grew at every step. */
  containers: Array<{ signature: string; counts: number[] }>
  /** user_class_count per dump, when every dump has it: code compiled at runtime is never freed. */
  userClasses: number[] | null
}

function growsEveryStep(values: number[]): boolean {
  for (let at = 1; at < values.length; at++) if (values[at]! <= values[at - 1]!) return false
  return values.length >= 2
}

/**
 * Unbounded growth over a series (memlab's unbound-object / unbound-collection):
 * what grows at every step from the oldest baseline to the current dump. Noise
 * that comes and goes does not grow every time; a leak does.
 */
export function growthSeries(baselines: Baseline[], store: DumpStore, name: string): SeriesResult {
  const current = reduceToSeriesPoint(store)
  const points = [...baselines.map((baseline) => ({ rows: baseline.rows, containers: baseline.containers ?? [], header: baseline.header })), current]
  const key = (row: { key: string; isClass: boolean }): string => (row.isClass ? 'class:' : 'type:') + row.key

  const series = new Map<string, SeriesRow>()
  points.forEach((point, at) => {
    for (const row of point.rows) {
      let entry = series.get(key(row))
      if (entry === undefined) {
        entry = { key: row.key, isClass: row.isClass, counts: new Array<number>(points.length).fill(0), sizes: new Array<number>(points.length).fill(0) }
        series.set(key(row), entry)
      }
      entry.counts[at] = row.count
      entry.sizes[at] = row.selfSize
    }
  })

  const containerSeries = new Map<string, number[]>()
  points.forEach((point, at) => {
    for (const container of point.containers) {
      const counts = containerSeries.get(container.signature) ?? new Array<number>(points.length).fill(0)
      counts[at] = container.children
      containerSeries.set(container.signature, counts)
    }
  })

  const userClasses = points.map((point) => point.header?.user_class_count)
  const result: SeriesResult = {
    names: [...baselines.map((baseline) => baseline.name), name],
    classes: [...series.values()]
      .filter((row) => growsEveryStep(row.counts))
      .sort((a, b) => b.counts.at(-1)! - b.counts[0]! - (a.counts.at(-1)! - a.counts[0]!)),
    containers: [...containerSeries.entries()]
      .filter(([, counts]) => growsEveryStep(counts))
      .map(([signature, counts]) => ({ signature, counts }))
      .sort((a, b) => b.counts.at(-1)! - b.counts[0]! - (a.counts.at(-1)! - a.counts[0]!)),
    userClasses: userClasses.every((count) => count !== undefined) ? (userClasses as number[]) : null,
  }
  log.info('growth series', { dumps: points.length, classes: result.classes.length, containers: result.containers.length })
  return result
}

function reduceToSeriesPoint(store: DumpStore): { rows: BaselineRow[]; containers: BaselineContainer[]; header: DumpHeader } {
  return {
    rows: summarize(store).map(({ key, isClass, count, selfSize }) => ({ key, isClass, count, selfSize })),
    containers: containers(store),
    header: store.header,
  }
}

/**
 * Three-dump leak detection (memlab, heapy): objects of the current dump that
 * did not exist in the first baseline, appeared by the second and are still
 * alive now. Temporaries of the second dump are gone by the current one, and
 * what the process held from the start is in the first: what is left is what
 * the work between the first two dumps left behind.
 *
 * Returns indices of the current store.
 */
export function leakCandidates(first: Baseline, second: Baseline, store: DumpStore): number[] {
  if (first.objects === undefined || second.objects === undefined) {
    throw new Error('the baselines were reduced without object identities; load them again')
  }
  const identity = (handle: number, className: string): string => `${handle}:${className}`
  const before = new Set<string>()
  for (let at = 0; at < first.objects.handles.length; at++) {
    before.add(identity(first.objects.handles[at]!, first.objects.classNames[first.objects.classes[at]!]!))
  }
  const appeared = new Set<string>()
  const handlesOfSecond = new Set<number>()
  for (let at = 0; at < second.objects.handles.length; at++) {
    const id = identity(second.objects.handles[at]!, second.objects.classNames[second.objects.classes[at]!]!)
    handlesOfSecond.add(second.objects.handles[at]!)
    if (!before.has(id)) appeared.add(id)
  }

  const candidates: number[] = []
  let reused = 0
  for (let i = 0; i < store.count; i++) {
    if (store.handle[i] === NONE || store.classId[i] === NONE) continue
    const id = identity(store.handle[i]!, store.classes.get(store.classId[i]!))
    if (appeared.has(id)) candidates.push(i)
    else if (handlesOfSecond.has(store.handle[i]!) && !before.has(id)) reused++
  }
  if (reused > 0) {
    // A handle of the second dump now holds an object of another class: freed and
    // reused in between. Those objects are not candidates; many of them means the
    // dumps are far apart, or the process churns through objects.
    log.warn('handles reused by another class between the dumps', { reused })
  }
  log.info('leak candidates', { appeared: appeared.size, candidates: candidates.length, reused })
  return candidates
}

