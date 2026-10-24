import { createLogger } from '../../log'
import { allocate } from '../columns'
import type { DumpStore } from '../store'
import type { Predecessors } from './graph'

const log = createLogger('dominators')

/**
 * Dominator tree and retained sizes.
 *
 * A virtual root (index = store.count) points at every `is_root` item; item d
 * dominates v when every path from the virtual root to v goes through d. The
 * retained size of v is the self size of everything v dominates: what would be
 * freed if v were. Cooper–Harvey–Kennedy's iterative algorithm, with an explicit
 * DFS stack — dumps nest 200 000 levels deep.
 *
 * Sizes are the dump's estimates and some roots may be missing from the dump
 * (see the format notes), so retained sizes are relative, not exact.
 *
 * Edges flagged in `ignored` (see filters.ts) are left out, as if the holder
 * did not reference the item: what else keeps it alive then shows.
 */
export interface Dominators {
  /** Immediate dominator of each item; -1 when unreachable; the virtual root is `count`. */
  idom: Int32Array
  retained: Float64Array
  virtualRoot: number
  reachable: number
  passes: number
  /** Items in reverse post-order of the DFS (dominators before what they dominate). */
  order: Int32Array
}

export function computeDominators(store: DumpStore, preds: Predecessors, ignored: Uint8Array | null = null): Dominators {
  const started = performance.now()
  const n = store.count
  const root = n
  const { childStart, edgeTarget } = store

  // --- DFS from the virtual root: post-order numbers ------------------------
  const postorder = allocate(Int32Array, n + 1, n).fill(-1)
  const order = allocate(Int32Array, n + 1, n) // post-order, reversed at the end
  const stack = allocate(Int32Array, n + 1, n)
  const cursor = allocate(Uint32Array, n + 1, n)
  const visited = allocate(Uint8Array, n + 1, n)

  // The virtual root's children are the roots, in dump order.
  const roots: number[] = []
  for (let i = 0; i < n; i++) {
    if (store.isRoot(i)) roots.push(i)
  }

  let counter = 0
  let depth = 0
  stack[0] = root
  cursor[0] = 0
  visited[root] = 1
  while (depth >= 0) {
    const node = stack[depth]!
    let next = -1
    if (node === root) {
      while (cursor[depth]! < roots.length) {
        const candidate = roots[cursor[depth]!++]!
        if (visited[candidate] === 0) {
          next = candidate
          break
        }
      }
    } else {
      const end = childStart[node + 1]!
      let edge = childStart[node]! + cursor[depth]!
      while (edge < end) {
        const candidate = edgeTarget[edge]!
        const skip = ignored !== null && ignored[edge] === 1
        edge++
        if (!skip && candidate >= 0 && visited[candidate] === 0) {
          next = candidate
          break
        }
      }
      cursor[depth] = edge - childStart[node]!
    }
    if (next >= 0) {
      visited[next] = 1
      depth++
      stack[depth] = next
      cursor[depth] = 0
    } else {
      postorder[node] = counter
      order[counter++] = node
      depth--
    }
  }
  const reachable = counter - 1
  order.subarray(0, counter).reverse()
  const rpo = order.subarray(0, counter)

  // --- iterate to the fixed point -------------------------------------------
  const idom = allocate(Int32Array, n + 1, n).fill(-1)
  idom[root] = root
  const { predStart, predSource, predEdge } = preds

  const intersect = (a: number, b: number): number => {
    while (a !== b) {
      while (postorder[a]! < postorder[b]!) a = idom[a]!
      while (postorder[b]! < postorder[a]!) b = idom[b]!
    }
    return a
  }

  let passes = 0
  let changed = true
  while (changed) {
    changed = false
    passes++
    for (let at = 1; at < rpo.length; at++) {
      const node = rpo[at]!
      let candidate = store.isRoot(node) ? root : -1
      for (let p = predStart[node]!; p < predStart[node + 1]!; p++) {
        const pred = predSource[p]!
        if (idom[pred] === -1 || (ignored !== null && ignored[predEdge[p]!] === 1)) continue
        candidate = candidate === -1 ? pred : intersect(pred, candidate)
      }
      if (idom[node] !== candidate) {
        idom[node] = candidate
        changed = true
      }
    }
  }

  // --- retained sizes: children before their dominator ------------------------
  const retained = allocate(Float64Array, n + 1, n)
  for (let i = 0; i < n; i++) {
    if (idom[i] !== -1) retained[i] = store.size[i]!
  }
  for (let at = rpo.length - 1; at >= 1; at--) {
    const node = rpo[at]!
    retained[idom[node]!]! += retained[node]!
  }

  log.info('dominators computed', {
    items: n,
    reachable,
    unreachable: n - reachable,
    passes,
    ms: Math.round(performance.now() - started),
  })

  return { idom, retained, virtualRoot: root, reachable, passes, order: rpo.slice() }
}
