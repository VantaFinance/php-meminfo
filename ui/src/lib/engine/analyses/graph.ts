import { createLogger } from '../../log'
import { allocate } from '../columns'
import type { DumpStore } from '../store'

const log = createLogger('graph')

/**
 * Incoming edges, as a CSR: the parents of item i are predSource[predStart[i]
 * … predStart[i + 1] - 1], each reached through edge predEdge[…] (its name).
 * Dangling edges are left out.
 */
export interface Predecessors {
  predStart: Uint32Array
  predSource: Int32Array
  predEdge: Int32Array
}

export function buildPredecessors(store: DumpStore): Predecessors {
  const started = performance.now()
  const n = store.count
  const predStart = allocate(Uint32Array, n + 1, n)

  for (let edge = 0; edge < store.edgeCount; edge++) {
    const target = store.edgeTarget[edge]!
    if (target >= 0) predStart[target + 1]!++
  }
  for (let i = 0; i < n; i++) {
    predStart[i + 1] = predStart[i + 1]! + predStart[i]!
  }

  const total = predStart[n]!
  const predSource = allocate(Int32Array, total, n)
  const predEdge = allocate(Int32Array, total, n)
  const fill = predStart.slice(0, n)

  for (let source = 0; source < n; source++) {
    for (let edge = store.childStart[source]!; edge < store.childStart[source + 1]!; edge++) {
      const target = store.edgeTarget[edge]!
      if (target < 0) continue
      const at = fill[target]!++
      predSource[at] = source
      predEdge[at] = edge
    }
  }

  log.debug('predecessors built', { edges: total, ms: Math.round(performance.now() - started) })
  return { predStart, predSource, predEdge }
}

/**
 * Multi-source BFS from every root along the children: the distance of each
 * item from its nearest root (-1 when unreachable) and the parent on that path.
 */
export interface RootDistances {
  distance: Int32Array
  parent: Int32Array
  parentEdge: Int32Array
  unreachable: number
}

export function rootDistances(store: DumpStore): RootDistances {
  const started = performance.now()
  const n = store.count
  const distance = allocate(Int32Array, n, n).fill(-1)
  const parent = allocate(Int32Array, n, n).fill(-1)
  const parentEdge = allocate(Int32Array, n, n).fill(-1)
  const queue = allocate(Int32Array, n, n)
  let head = 0
  let tail = 0

  for (let i = 0; i < n; i++) {
    if (store.isRoot(i)) {
      distance[i] = 0
      queue[tail++] = i
    }
  }

  while (head < tail) {
    const item = queue[head++]!
    const next = distance[item]! + 1
    for (let edge = store.childStart[item]!; edge < store.childStart[item + 1]!; edge++) {
      const child = store.edgeTarget[edge]!
      if (child >= 0 && distance[child] === -1) {
        distance[child] = next
        parent[child] = item
        parentEdge[child] = edge
        queue[tail++] = child
      }
    }
  }

  const unreachable = n - tail
  log.debug('root distances', { reachable: tail, unreachable, ms: Math.round(performance.now() - started) })
  return { distance, parent, parentEdge, unreachable }
}
