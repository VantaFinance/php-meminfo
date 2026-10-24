import { createLogger } from '../../log'
import { NONE, type DumpStore } from '../store'
import type { Dominators } from './dominators'
import type { RootDistances } from './graph'

const log = createLogger('retainers')

/**
 * How the instances of a class are held, grouped.
 *
 *  - clusters: instances whose shortest path from a root has the same shape —
 *    the same root, keys and kinds of holder, integer keys collapsed to "[]" —
 *    with one representative each (memlab's leak clusters, dotMemory's similar
 *    retention). Thousands of leaked objects usually fall into a few clusters,
 *    one per place in the code that keeps them.
 *  - merged paths: the same shortest paths as one tree from the roots down,
 *    with the instance count under each node (Eclipse MAT).
 *  - immediate dominators: what alone keeps each instance alive, grouped by its
 *    class; holders matching `skip` (framework internals) are passed over to
 *    their own dominator.
 */

export interface RetainerCluster {
  /** Root first: "<GLOBAL> $cache", then "[] → array", "->items → Foo"… */
  steps: string[]
  count: number
  retained: number
  representative: number
}

export interface MergedPathNode {
  step: string
  count: number
  children: MergedPathNode[]
}

export interface DominatorGroup {
  key: string
  members: number
  retained: number
  /** One of the holders, -1 for the virtual root (data shared by several roots). */
  representative: number
}

export interface RetainerReport {
  instances: number
  clusters: RetainerCluster[]
  merged: MergedPathNode
  dominators: DominatorGroup[]
}

/** Path steps beyond this are cut: "…" stands for the rest towards the root. */
const MAX_DEPTH = 32
const MAX_CLUSTERS = 50
const MAX_GROUPS = 50
/** Children kept per node of the merged tree, and nodes overall. */
const MERGED_BREADTH = 20
const MERGED_NODES = 500

export function kindOf(store: DumpStore, index: number): string {
  return store.className(index) ?? store.typeName(index)
}

/** Items of a class (isClass) or of a type. */
export function instancesOf(store: DumpStore, key: string, isClass: boolean): number[] {
  const members: number[] = []
  if (isClass) {
    const id = store.classes.find(key)
    if (id === NONE) return members
    for (let i = 0; i < store.count; i++) if (store.classId[i] === id) members.push(i)
    return members
  }
  const type = store.types.find(key)
  if (type === NONE) return members
  for (let i = 0; i < store.count; i++) if (store.type[i] === type && store.classId[i] === NONE) members.push(i)
  return members
}

/** The shortest root path of an item as normalised steps, root first. */
export function pathSignature(store: DumpStore, distances: RootDistances, index: number): string[] {
  if (distances.distance[index]! < 0) return ['(not reachable from a root)']
  const steps: string[] = []
  let node = index
  while (distances.parent[node]! >= 0) {
    if (steps.length === MAX_DEPTH) {
      steps.push('…')
      return steps.reverse()
    }
    const edge = distances.parentEdge[node]!
    const name = store.edgeNameText(edge)
    const key = /^-?\d+$/.test(name) ? '[]' : `->${name}`
    steps.push(`${key} → ${kindOf(store, node)}`)
    node = distances.parent[node]!
  }
  const frame = store.frameId[node]! === NONE ? '' : store.frames.get(store.frameId[node]!)
  const symbol = store.symbolId[node]! === NONE ? '' : ` $${store.symbols.get(store.symbolId[node]!)}`
  steps.push(`${frame}${symbol} ${kindOf(store, node)}`.trim())
  return steps.reverse()
}

export function analyzeRetainers(
  store: DumpStore,
  distances: RootDistances,
  dominators: Dominators | null,
  members: number[],
  skip: RegExp | null,
): RetainerReport {
  const started = performance.now()
  const retainedOf = (i: number): number => (dominators !== null && dominators.idom[i] !== -1 ? dominators.retained[i]! : store.size[i]!)

  // --- clusters and merged tree ---------------------------------------------
  const clusters = new Map<string, RetainerCluster>()
  const root: MergedPathNode = { step: '(roots)', count: 0, children: [] }
  const childIndex = new Map<MergedPathNode, Map<string, MergedPathNode>>()
  for (const member of members) {
    const steps = pathSignature(store, distances, member)
    const signature = steps.join('\n')
    const cluster = clusters.get(signature)
    if (cluster === undefined) {
      clusters.set(signature, { steps, count: 1, retained: retainedOf(member), representative: member })
    } else {
      cluster.count++
      cluster.retained += retainedOf(member)
      if (retainedOf(member) > retainedOf(cluster.representative)) cluster.representative = member
    }

    root.count++
    let node: MergedPathNode = root
    for (const step of steps) {
      let index = childIndex.get(node)
      if (index === undefined) {
        index = new Map()
        childIndex.set(node, index)
      }
      let next = index.get(step)
      if (next === undefined) {
        next = { step, count: 0, children: [] }
        index.set(step, next)
        node.children.push(next)
      }
      next.count++
      node = next
    }
  }

  // --- immediate dominators, skipping framework holders ---------------------
  const groups = new Map<string, DominatorGroup>()
  if (dominators !== null) {
    for (const member of members) {
      let holder = dominators.idom[member]!
      if (holder === -1) continue
      while (skip !== null && holder !== dominators.virtualRoot && skip.test(kindOf(store, holder))) {
        holder = dominators.idom[holder]!
      }
      const key = holder === dominators.virtualRoot ? '(several roots, or a root itself)' : kindOf(store, holder)
      const group = groups.get(key) ?? { key, members: 0, retained: 0, representative: holder === dominators.virtualRoot ? -1 : holder }
      group.members++
      group.retained += retainedOf(member)
      groups.set(key, group)
    }
  }

  const report: RetainerReport = {
    instances: members.length,
    clusters: [...clusters.values()].sort((a, b) => b.count - a.count || b.retained - a.retained).slice(0, MAX_CLUSTERS),
    merged: prune(root),
    dominators: [...groups.values()].sort((a, b) => b.members - a.members || b.retained - a.retained).slice(0, MAX_GROUPS),
  }
  log.info('retainers', {
    instances: members.length,
    clusters: clusters.size,
    groups: groups.size,
    ms: Math.round(performance.now() - started),
  })
  return report
}

/** Keep the busiest children of each node, breadth first, up to MERGED_NODES nodes. */
function prune(root: MergedPathNode): MergedPathNode {
  const out: MergedPathNode = { step: root.step, count: root.count, children: [] }
  const queue: Array<[MergedPathNode, MergedPathNode]> = [[root, out]]
  let nodes = 1
  while (queue.length > 0 && nodes < MERGED_NODES) {
    const [from, to] = queue.shift()!
    const children = [...from.children].sort((a, b) => b.count - a.count).slice(0, MERGED_BREADTH)
    for (const child of children) {
      if (nodes >= MERGED_NODES) break
      const copy: MergedPathNode = { step: child.step, count: child.count, children: [] }
      to.children.push(copy)
      queue.push([child, copy])
      nodes++
    }
  }
  return out
}
