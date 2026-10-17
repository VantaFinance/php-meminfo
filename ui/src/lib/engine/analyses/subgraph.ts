import { createLogger } from '../../log'
import type { DumpStore } from '../store'
import { referrers, type ReferencePathFinder } from './basic'
import type { Dominators } from './dominators'
import type { Predecessors } from './graph'
import { dominatorChain } from './suspects'

/**
 * Small, bounded pieces of the dump graph to draw. Only these ever leave the
 * worker: a whole dump is millions of nodes, a picture is a few hundred.
 */

const log = createLogger('subgraph')

export interface SubgraphEdge {
  source: number
  target: number
  /** Index of the edge in the store, for its name. */
  edge: number
}

export interface Subgraph {
  nodes: number[]
  edges: SubgraphEdge[]
  /** The cap was reached: the picture is not complete. */
  truncated: boolean
  /** Roots that reach the target (paths mode); 0 in neighbourhood mode. */
  totalRoots: number
  /** Nodes every path from a root to the target goes through (its dominators), within `nodes`. */
  bottlenecks: number[]
}

class Builder {
  readonly nodes: number[] = []
  readonly edges: SubgraphEdge[] = []
  private readonly seenNodes = new Set<number>()
  private readonly seenEdges = new Set<number>()

  hasNode(index: number): boolean {
    return this.seenNodes.has(index)
  }

  addNode(index: number): void {
    if (this.seenNodes.has(index)) return
    this.seenNodes.add(index)
    this.nodes.push(index)
  }

  addEdge(source: number, target: number, edge: number): void {
    if (this.seenEdges.has(edge)) return
    this.seenEdges.add(edge)
    this.edges.push({ source, target, edge })
  }

  get size(): number {
    return this.nodes.length
  }
}

/**
 * Every shortest path from a root to `target` (one per root, nearest root
 * first — ReferencePathFinder's order), merged, until `maxNodes`. A path is
 * added whole or not at all, so every drawn path really reaches a root.
 */
export function pathsSubgraph(
  finder: ReferencePathFinder,
  dominators: Dominators | null,
  target: number,
  maxNodes: number,
): Subgraph {
  const started = performance.now()
  const builder = new Builder()
  builder.addNode(target)

  // Every path brings at least its own root, so more than maxNodes of them can
  // never fit.
  const { paths, rootsReaching } = finder.find(target, maxNodes)
  let truncated = false

  for (const path of paths) {
    if (path.self) continue
    const fresh = path.steps.filter((step) => !builder.hasNode(step.item)).length
    if (builder.size + fresh > maxNodes) {
      truncated = true
      continue // a shorter one may still fit
    }
    for (let at = 0; at < path.steps.length; at++) {
      const step = path.steps[at]!
      builder.addNode(step.item)
      if (at > 0) builder.addEdge(path.steps[at - 1]!.item, step.item, step.edge)
    }
  }

  // The bottleneck is a dominator of the target, never something worked out on
  // a capped picture: by definition every path from a root goes through it.
  const bottlenecks = dominators === null
    ? []
    : dominatorChain(dominators, target).filter((node) => node !== target && builder.hasNode(node))

  log.debug('paths subgraph', {
    nodes: builder.size,
    edges: builder.edges.length,
    roots: rootsReaching,
    truncated,
    ms: Math.round(performance.now() - started),
  })
  return { nodes: builder.nodes, edges: builder.edges, truncated, totalRoots: rootsReaching, bottlenecks }
}

export type Direction = 'children' | 'parents' | 'both'

/**
 * Items within `depth` hops of `center`, following children, parents or both,
 * until `maxNodes`; with every edge between two included items.
 */
export function neighbourhoodSubgraph(
  store: DumpStore,
  preds: Predecessors,
  center: number,
  depth: number,
  maxNodes: number,
  direction: Direction,
): Subgraph {
  const started = performance.now()
  const builder = new Builder()
  builder.addNode(center)
  let frontier = [center]
  let next: number[] = []
  let truncated = false

  const visit = (node: number): boolean => {
    if (builder.hasNode(node)) return true
    if (builder.size >= maxNodes) {
      truncated = true
      return false
    }
    builder.addNode(node)
    next.push(node)
    return true
  }

  for (let hop = 0; hop < depth && frontier.length > 0 && !truncated; hop++) {
    next = []
    for (const node of frontier) {
      if (direction !== 'parents') {
        for (let edge = store.childStart[node]!; edge < store.childStart[node + 1]!; edge++) {
          const child = store.edgeTarget[edge]!
          if (child >= 0 && !visit(child)) break
        }
      }
      if (direction !== 'children') {
        for (const { source } of referrers(preds, node)) {
          if (!visit(source)) break
        }
      }
      if (truncated) break
    }
    frontier = next
  }

  // Every edge between included items, in both directions.
  for (const node of builder.nodes) {
    for (let edge = store.childStart[node]!; edge < store.childStart[node + 1]!; edge++) {
      const child = store.edgeTarget[edge]!
      if (child >= 0 && builder.hasNode(child)) builder.addEdge(node, child, edge)
    }
  }

  log.debug('neighbourhood subgraph', {
    nodes: builder.size,
    edges: builder.edges.length,
    depth,
    direction,
    truncated,
    ms: Math.round(performance.now() - started),
  })
  return { nodes: builder.nodes, edges: builder.edges, truncated, totalRoots: 0, bottlenecks: [] }
}
