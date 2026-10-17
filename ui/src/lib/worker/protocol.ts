import type { LogLevel } from '../log'
import type { QueryFilter, SummaryRow } from '../engine/analyses/basic'
import type { Baseline, GrowthRow } from '../engine/analyses/diff'
import type { DumpHeader, StoreStats } from '../engine/store'

/**
 * Messages between the page and a dump worker.
 *
 * Only pages of results cross this boundary — never the store. Everything the
 * page shows is built from these small, structured-clonable values.
 */

/* --- rows ------------------------------------------------------------------ */

/** One item as shown in a table. */
export interface ItemRow {
  index: number
  address: string
  type: string
  className?: string
  symbol?: string
  frame?: string
  size: number
  isRoot: boolean
  /** Edges, duplicates included. */
  children: number
  /** Present once the dominator tree is computed. */
  retained?: number
}

export interface Overview {
  fileName: string
  fileSize: number
  header: DumpHeader
  stats: StoreStats
  roots: number
  totalSize: number
  parseMs: number
}

export interface Analysis {
  reachable: number
  sharedItems: number
  analyzeMs: number
}

export interface SuspectRow extends ItemRow {
  retained: number
  shared: boolean
  dominated: number
}

export interface ClassRetentionRow {
  key: string
  isClass: boolean
  instances: number
  topMost: number
  retained: number
}

export interface SuspectsResult {
  suspects: SuspectRow[]
  classes: ClassRetentionRow[]
  frames: Array<{ frame: string; roots: number; retained: number }>
  totalSize: number
  sharedItems: number
}

export interface TopChildrenRowView extends ItemRow {
  uniqueKeys: number
}

export interface QueryPage {
  rows: ItemRow[]
  total: number
  offset: number
}

export interface ItemDetail {
  row: ItemRow
  objectHandle?: number
  /** Dominated by no root but by several — data shared by several roots. */
  shared: boolean
  /** Items it dominates, itself included. */
  dominated?: number
  /** Outermost first, ending with this item. */
  dominatorChain: ItemRow[]
  /** Distance from the nearest root; -1 when no root reaches it. */
  distanceFromRoot: number
  referrers: number
}

export interface EdgePage {
  /** For children: the child; for referrers: the parent. */
  rows: Array<{ name: string; item: ItemRow | null }>
  total: number
  offset: number
}

export type GraphMode = 'paths' | 'neighbourhood'
export type GraphDirection = 'children' | 'parents' | 'both'

export interface GraphNode extends ItemRow {
  isTarget: boolean
  /** Held by several roots, retained by none of them alone. */
  isShared: boolean
  /** Every path from a root to the target goes through it. */
  isBottleneck: boolean
}

export interface GraphResult {
  target: number
  mode: GraphMode
  nodes: GraphNode[]
  edges: Array<{ source: number; target: number; name: string }>
  truncated: boolean
  /** Roots reaching the target (paths mode). */
  totalRoots: number
}

export interface PathsResult {
  paths: Array<{ self: boolean; steps: Array<{ name: string | null; item: ItemRow }> }>
  rootsReaching: number
}

/* --- requests ------------------------------------------------------------ */

export type Request =
  | { kind: 'load'; file: File }
  | { kind: 'loadBaseline'; file: File }
  | { kind: 'analyze' }
  | { kind: 'summary' }
  | { kind: 'suspects'; limit: number }
  | { kind: 'topChildren'; limit: number }
  | { kind: 'query'; filters: QueryFilter[]; offset: number; limit: number }
  | { kind: 'item'; index: number }
  | { kind: 'children'; index: number; offset: number; limit: number }
  | { kind: 'referrers'; index: number; offset: number; limit: number }
  | { kind: 'paths'; index: number; max: number }
  | { kind: 'graph'; index: number; mode: GraphMode; depth: number; direction: GraphDirection; maxNodes: number }
  | { kind: 'diff'; baseline: Baseline }
  | { kind: 'find'; address: string }

export interface ResponseMap {
  load: Overview
  loadBaseline: Baseline
  analyze: Analysis
  summary: SummaryRow[]
  suspects: SuspectsResult
  topChildren: TopChildrenRowView[]
  query: QueryPage
  item: ItemDetail
  children: EdgePage
  referrers: EdgePage
  paths: PathsResult
  graph: GraphResult
  diff: GrowthRow[]
  find: number
}

export type ErrorKind = 'invalid-dump' | 'too-large' | 'cancelled' | 'invalid-query' | 'no-dump' | 'worker-died' | 'internal'

export interface WorkerError {
  kind: ErrorKind
  message: string
}

/** Page → worker. */
export type ToWorker =
  | { type: 'init'; logLevel: LogLevel }
  | { type: 'request'; id: number; request: Request }

export type Phase = 'reading' | 'resolving' | 'analyzing'

/** Worker → page. */
export type FromWorker =
  | { type: 'progress'; id: number; phase: Phase; bytes: number; totalBytes: number; items: number }
  | { type: 'result'; id: number; ok: true; value: unknown }
  | { type: 'result'; id: number; ok: false; error: WorkerError }
