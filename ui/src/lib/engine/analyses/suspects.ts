import { createLogger } from '../../log'
import { allocate } from '../columns'
import { NONE, type DumpStore } from '../store'
import type { Dominators } from './dominators'
import { TopK } from './topk'

const log = createLogger('suspects')

/**
 * Leak suspects from the dominator tree.
 *
 * Ranking items by retained size alone is misleading on real dumps: a linked
 * chain of 200 000 nodes makes every node "retain" most of the chain, and data
 * held by several roots is dominated by none of them and so retained by
 * nobody. Hence:
 *  - a suspect absorbed by its dominator (which retains barely more) is folded
 *    into it — chains collapse to their head;
 *  - items dominated directly by the virtual root without being roots are data
 *    shared by several roots: flagged as such;
 *  - classes are ranked by what their top-most instances retain, so a class
 *    nested in itself is not counted twice.
 */

/**
 * An item that accounts for at least this share of what its dominator retains
 * beyond the dominator itself is folded into it (a chain link, or the only
 * payload of a wrapper).
 */
const ABSORBED_SHARE = 0.9

/**
 * Below this share of the dump, an item that is neither a root nor shared is
 * left to the per-class table: one of 200 000 leaked objects is noise on its
 * own, and the class row already sums them.
 */
const MIN_SHARE = 0.01

export interface Suspect {
  index: number
  retained: number
  /** Shared by several roots: dominated by none of them. */
  shared: boolean
  /** How many items it dominates (itself included). */
  dominated: number
}

export interface ClassRetention {
  /** Type, or class for objects. */
  key: string
  isClass: boolean
  instances: number
  /** Instances not dominated by another instance of the same class. */
  topMost: number
  retained: number
}

export interface FrameRetention {
  frame: string
  roots: number
  retained: number
}

export interface SuspectReport {
  suspects: Suspect[]
  classes: ClassRetention[]
  frames: FrameRetention[]
  /** Sum of the self sizes the dump reports (an estimate of the real usage). */
  totalSize: number
  sharedItems: number
  /** Items each item dominates, itself included (index `count` is the virtual root). */
  dominatedCounts: Uint32Array
}

export function findSuspects(store: DumpStore, dominators: Dominators, limit = 100): SuspectReport {
  const started = performance.now()
  const n = store.count
  const { idom, retained, virtualRoot, order } = dominators

  // Dominator tree as a CSR, children grouped under their dominator.
  const childCount = allocate(Uint32Array, n + 2, n)
  for (let i = 0; i < n; i++) {
    if (idom[i] !== -1) childCount[idom[i]! + 1]!++
  }
  for (let i = 0; i <= n; i++) childCount[i + 1] = childCount[i + 1]! + childCount[i]!
  const treeStart = childCount
  const treeChild = allocate(Int32Array, treeStart[n + 1]!, n)
  const fill = treeStart.slice(0, n + 1)
  for (const node of order) {
    if (node !== virtualRoot) treeChild[fill[idom[node]!]!++] = node
  }

  // Sizes of the dominated subtrees, children before parents.
  const dominated = allocate(Uint32Array, n + 1, n)
  for (let at = order.length - 1; at >= 0; at--) {
    const node = order[at]!
    dominated[node] = dominated[node]! + 1
    if (node !== virtualRoot) dominated[idom[node]!] = dominated[idom[node]!]! + dominated[node]!
  }

  // --- individual suspects ---------------------------------------------------
  const top = new TopK(limit)
  let sharedItems = 0
  let totalSize = 0
  for (let i = 0; i < n; i++) totalSize += store.size[i]!
  const minRetained = totalSize * MIN_SHARE
  for (let i = 0; i < n; i++) {
    const dominator = idom[i]!
    if (dominator === -1) continue
    if (dominator === virtualRoot) {
      // Roots, and data shared by several roots: always candidates.
      if (!store.isRoot(i)) sharedItems++
    } else if (retained[i]! < minRetained || retained[i]! >= ABSORBED_SHARE * (retained[dominator]! - store.size[dominator]!)) {
      continue
    }
    top.push(retained[i]!, i)
  }
  const suspects = top.result().map(({ score, index }) => ({
    index,
    retained: score,
    shared: idom[index] === virtualRoot && !store.isRoot(index),
    dominated: dominated[index]!,
  }))

  // --- classes, by top-most instance --------------------------------------
  const types = store.types.size
  const groups = types + store.classes.size
  const objectType = store.types.find('object')
  const groupOf = (i: number): number => {
    const classId = store.classId[i]!
    return store.type[i] === objectType && classId !== NONE ? types + classId : store.type[i]!
  }
  const instances = new Float64Array(groups)
  const topMost = new Float64Array(groups)
  const groupRetained = new Float64Array(groups)
  const active = new Uint32Array(groups)

  // Iterative DFS over the dominator tree, counting open ancestors per group.
  const stack = allocate(Int32Array, n + 1, n)
  const cursor = allocate(Uint32Array, n + 1, n)
  let depth = 0
  stack[0] = virtualRoot
  cursor[0] = treeStart[virtualRoot]!
  while (depth >= 0) {
    const node = stack[depth]!
    if (cursor[depth]! < treeStart[node + 1]!) {
      const child = treeChild[cursor[depth]!++]!
      const group = groupOf(child)
      instances[group]!++
      if (active[group] === 0) {
        topMost[group]!++
        groupRetained[group]! += retained[child]!
      }
      active[group]!++
      depth++
      stack[depth] = child
      cursor[depth] = treeStart[child]!
    } else {
      if (node !== virtualRoot) active[groupOf(node)]!--
      depth--
    }
  }

  const classes: ClassRetention[] = []
  for (let group = 0; group < groups; group++) {
    if (instances[group] === 0) continue
    const isClass = group >= types
    classes.push({
      key: isClass ? store.classes.get(group - types) : store.types.get(group),
      isClass,
      instances: instances[group]!,
      topMost: topMost[group]!,
      retained: groupRetained[group]!,
    })
  }
  classes.sort((a, b) => b.retained - a.retained || b.instances - a.instances || a.key.localeCompare(b.key))

  // --- roots by frame ---------------------------------------------------------
  const frameRoots = new Map<number, { roots: number; retained: number }>()
  for (let at = treeStart[virtualRoot]!; at < treeStart[virtualRoot + 1]!; at++) {
    const child = treeChild[at]!
    if (!store.isRoot(child)) continue
    const frame = store.frameId[child]!
    const entry = frameRoots.get(frame) ?? { roots: 0, retained: 0 }
    entry.roots++
    entry.retained += retained[child]!
    frameRoots.set(frame, entry)
  }
  const frames = [...frameRoots.entries()]
    .map(([frame, entry]) => ({ frame: frame === NONE ? '(no frame)' : store.frames.get(frame), ...entry }))
    .sort((a, b) => b.retained - a.retained || a.frame.localeCompare(b.frame))

  log.info('suspects ranked', {
    suspects: suspects.length,
    sharedItems,
    classes: classes.length,
    ms: Math.round(performance.now() - started),
  })

  return { suspects, classes, frames, totalSize, sharedItems, dominatedCounts: dominated }
}

/**
 * The chain of dominators from the virtual root down to `index`: what keeps it
 * alive, outermost first.
 */
export function dominatorChain(dominators: Dominators, index: number, max = 64): number[] {
  const chain: number[] = []
  let node = index
  while (node !== -1 && node !== dominators.virtualRoot && chain.length < max) {
    chain.push(node)
    node = dominators.idom[node]!
  }
  return chain.reverse()
}
