import { createLogger } from '../log'
import { InvalidQueryError, query, ReferencePathFinder, referrers, summarize, topChildren } from '../engine/analyses/basic'
import { compareWithBaseline, growthSeries, leakCandidates, reduceToBaseline, type Baseline } from '../engine/analyses/diff'
import { computeDominators, type Dominators } from '../engine/analyses/dominators'
import { findFindings } from '../engine/analyses/findings'
import { generationTable } from '../engine/analyses/generations'
import { allocationSites } from '../engine/analyses/sites'
import { histogramTrend, InvalidHistogramError, parseHistogram } from '../engine/analyses/histogram'
import { ignoredEdges, InvalidFilterError, type RetainerFilter } from '../engine/analyses/filters'
import { analyzeRetainers, instancesOf, type RetainerReport } from '../engine/analyses/retainers'
import { buildPredecessors, rootDistances, type Predecessors, type RootDistances } from '../engine/analyses/graph'
import { neighbourhoodSubgraph, pathsSubgraph } from '../engine/analyses/subgraph'
import { dominatorChain, findSuspects, type SuspectReport } from '../engine/analyses/suspects'
import { DumpTooLargeError } from '../engine/columns'
import { DumpParseError, parseDumpStream } from '../engine/parser'
import type { DumpStore } from '../engine/store'
import type {
  EdgePage,
  ErrorKind,
  FindingRow,
  ItemDetail,
  ItemRow,
  Phase,
  RetainersResult,
  Request,
  ResponseMap,
  WorkerError,
} from './protocol'

const log = createLogger('session')

const PROGRESS_INTERVAL_MS = 100
const MAX_PAGE = 1000
/** Beyond this a picture is unreadable, and its labels a real cost on the page. */
const MAX_GRAPH_NODES = 1000

export type ProgressSink = (phase: Phase, bytes: number, totalBytes: number, items: number) => void

/** Maps anything thrown to what the page shows. */
export function toWorkerError(error: unknown): WorkerError {
  const message = error instanceof Error ? error.message : String(error)
  let kind: ErrorKind = 'internal'
  if (error instanceof DumpParseError) kind = 'invalid-dump'
  else if (error instanceof DumpTooLargeError) kind = 'too-large'
  else if (error instanceof InvalidQueryError || error instanceof InvalidFilterError || error instanceof InvalidHistogramError) kind = 'invalid-query'
  else if (error instanceof DOMException && error.name === 'AbortError') kind = 'cancelled'
  else if (error instanceof NoDumpError) kind = 'no-dump'
  return { kind, message }
}

class NoDumpError extends Error {
  constructor() {
    super('no dump is loaded')
  }
}

/**
 * Everything one dump worker holds: the store and what is derived from it,
 * built lazily. Lives and dies with its worker.
 */
export class DumpSession {
  private store: DumpStore | null = null
  private preds: Predecessors | null = null
  private distances: RootDistances | null = null
  private dominators: Dominators | null = null
  private suspects: SuspectReport | null = null
  private finder: ReferencePathFinder | null = null
  /** Edges left out by the retainer filters (see filters.ts), null when none apply. */
  private ignored: Uint8Array | null = null
  private fileName = ''
  /** Computed on the first request, dropped whenever the dominators are recomputed. */
  private findings: FindingRow[] | null = null

  async handle<K extends Request['kind']>(request: Extract<Request, { kind: K }>, progress: ProgressSink): Promise<ResponseMap[K]> {
    return (await this.dispatch(request as Request, progress)) as ResponseMap[K]
  }

  private async dispatch(request: Request, progress: ProgressSink): Promise<ResponseMap[Request['kind']]> {
    switch (request.kind) {
      case 'load':
        return this.load(request.file, progress)
      case 'loadBaseline': {
        const store = await this.parse(request.file, progress)
        return reduceToBaseline(store, request.file.name)
      }
      case 'analyze':
        return this.analyze(progress)
      case 'setFilters':
        return this.setFilters(request.filters, progress)
      case 'summary':
        return summarize(this.requireStore())
      case 'suspects':
        return this.suspectsResult(request.limit)
      case 'findings':
        return this.findingsResult()
      case 'generations':
        return generationTable(this.requireStore())
      case 'sites':
        return allocationSites(this.requireStore(), this.dominators)
      case 'histograms':
        return histogramTrend(
          await Promise.all(request.files.map(async (file) => parseHistogram(await file.text(), file.name, file.lastModified))),
        )
      case 'retainers':
        return this.retainers(request.key, request.isClass, request.skip)
      case 'topChildren': {
        const store = this.requireStore()
        return topChildren(store, Math.min(request.limit, MAX_PAGE)).map((row) => ({ ...this.row(row.index), uniqueKeys: row.uniqueKeys }))
      }
      case 'query': {
        const store = this.requireStore()
        const result = query(store, request.filters, request.offset, Math.min(request.limit, MAX_PAGE))
        return { rows: result.indices.map((index) => this.row(index)), total: result.total, offset: request.offset }
      }
      case 'item':
        return this.item(request.index)
      case 'children':
        return this.children(request.index, request.offset, Math.min(request.limit, MAX_PAGE))
      case 'referrers':
        return this.referrerPage(request.index, request.offset, Math.min(request.limit, MAX_PAGE))
      case 'paths':
        return this.paths(request.index, Math.min(request.max, 200))
      case 'graph':
        return this.graph(request)
      case 'diff':
        return compareWithBaseline(request.baseline, this.requireStore())
      case 'series':
        return growthSeries(request.baselines, this.requireStore(), this.fileName)
      case 'leaks':
        return this.leaks(request.first, request.second)
      case 'find':
        return this.requireStore().findAddress(request.address.trim().toLowerCase())
    }
  }

  private async parse(file: File, progress: ProgressSink): Promise<DumpStore> {
    let last = 0
    return parseDumpStream(file.stream(), {
      expectedBytes: file.size,
      onProgress: ({ bytes, items }) => {
        const now = performance.now()
        if (now - last >= PROGRESS_INTERVAL_MS || bytes === file.size) {
          last = now
          progress(bytes === file.size ? 'resolving' : 'reading', bytes, file.size, items)
        }
      },
    })
  }

  private async load(file: File, progress: ProgressSink): Promise<ResponseMap['load']> {
    const started = performance.now()
    log.info('loading', { size: file.size })
    const store = await this.parse(file, progress)
    this.store = store
    this.fileName = file.name
    log.info('dump models', { identity: store.header.identity_model ?? 1, size: store.header.size_model ?? 1 })
    let roots = 0
    let totalSize = 0
    for (let i = 0; i < store.count; i++) {
      if (store.isRoot(i)) roots++
      totalSize += store.size[i]!
    }
    return {
      fileName: file.name,
      fileSize: file.size,
      header: store.header,
      stats: store.stats(),
      roots,
      totalSize,
      parseMs: Math.round(performance.now() - started),
    }
  }

  private analyze(progress: ProgressSink): ResponseMap['analyze'] {
    const store = this.requireStore()
    const started = performance.now()
    progress('analyzing', 0, 1, store.count)
    this.ensureGraph()
    this.dominators = computeDominators(store, this.preds!, this.ignored)
    this.findings = null
    this.suspects = findSuspects(store, this.dominators, MAX_PAGE)
    progress('analyzing', 1, 1, store.count)
    return {
      reachable: this.dominators.reachable,
      sharedItems: this.suspects.sharedItems,
      analyzeMs: Math.round(performance.now() - started),
    }
  }

  private leaks(first: Baseline, second: Baseline): ResponseMap['leaks'] {
    const store = this.requireStore()
    this.ensureGraph()
    const candidates = leakCandidates(first, second, store)
    const byClass = new Map<string, { key: string; count: number; retained: number }>()
    for (const index of candidates) {
      const key = store.className(index) ?? store.typeName(index)
      const entry = byClass.get(key) ?? { key, count: 0, retained: 0 }
      entry.count++
      if (this.dominators !== null && this.dominators.idom[index] !== -1) entry.retained += this.dominators.retained[index]!
      byClass.set(key, entry)
    }
    return {
      candidates: candidates.length,
      byClass: [...byClass.values()].sort((a, b) => b.count - a.count),
      retainers: this.retainersOf(analyzeRetainers(store, this.distances!, this.dominators, candidates, null)),
    }
  }

  private retainers(key: string, isClass: boolean, skip: string): ResponseMap['retainers'] {
    const store = this.requireStore()
    let pattern: RegExp | null = null
    if (skip.trim() !== '') {
      try {
        pattern = new RegExp(skip)
      } catch (error) {
        throw new InvalidQueryError(`invalid skip pattern: ${error instanceof Error ? error.message : String(error)}`)
      }
    }
    this.ensureGraph()
    return this.retainersOf(analyzeRetainers(store, this.distances!, this.dominators, instancesOf(store, key, isClass), pattern))
  }

  private retainersOf(report: RetainerReport): RetainersResult {
    return {
      instances: report.instances,
      clusters: report.clusters.map(({ representative, ...cluster }) => ({ ...cluster, item: this.row(representative) })),
      merged: report.merged,
      dominators: report.dominators.map(({ representative, ...group }) => ({
        ...group,
        item: representative < 0 ? null : this.row(representative),
      })),
    }
  }

  private findingsResult(): ResponseMap['findings'] {
    if (this.suspects === null || this.dominators === null) {
      throw new Error('the dump has not been analyzed yet')
    }
    this.findings ??= findFindings(this.requireStore(), this.dominators, this.suspects, this.preds!).map(({ index, ...finding }) => ({
      ...finding,
      item: index < 0 ? null : this.row(index),
    }))
    return this.findings
  }

  /** Apply retainer filters and recompute everything derived from reachability. */
  private setFilters(filters: RetainerFilter[], progress: ProgressSink): ResponseMap['setFilters'] {
    const store = this.requireStore()
    this.ignored = ignoredEdges(store, filters)
    this.distances = null
    this.finder = null
    log.info('filters set', { filters: filters.length })
    return this.analyze(progress)
  }

  private suspectsResult(limit: number): ResponseMap['suspects'] {
    if (this.suspects === null) {
      throw new Error('the dump has not been analyzed yet')
    }
    const report = this.suspects
    return {
      suspects: report.suspects.slice(0, limit).map((suspect) => ({
        ...this.row(suspect.index),
        retained: suspect.retained,
        shared: suspect.shared,
        dominated: suspect.dominated,
      })),
      classes: report.classes.slice(0, limit),
      frames: report.frames.slice(0, limit),
      totalSize: report.totalSize,
      sharedItems: report.sharedItems,
    }
  }

  private item(index: number): ItemDetail {
    const store = this.requireStore()
    this.checkIndex(index)
    this.ensureGraph()
    const view = store.item(index)
    const dominators = this.dominators
    const detail: ItemDetail = {
      row: this.row(index),
      shared: dominators !== null && dominators.idom[index] === dominators.virtualRoot && !store.isRoot(index),
      dominatorChain: dominators === null ? [] : dominatorChain(dominators, index).map((i) => this.row(i)),
      distanceFromRoot: this.distances!.distance[index]!,
      referrers: this.preds!.predStart[index + 1]! - this.preds!.predStart[index]!,
    }
    if (view.handle !== undefined) detail.objectHandle = view.handle
    if (view.extras !== undefined) detail.extras = view.extras
    if (this.suspects !== null) {
      detail.dominated = this.suspects.dominatedCounts[index]!
    }
    return detail
  }

  private children(index: number, offset: number, limit: number): EdgePage {
    const store = this.requireStore()
    this.checkIndex(index)
    const start = store.childStart[index]!
    const end = store.childStart[index + 1]!
    const rows: EdgePage['rows'] = []
    for (let edge = start + offset; edge < end && rows.length < limit; edge++) {
      const target = store.edgeTarget[edge]!
      rows.push({ name: store.edgeNameText(edge), item: target < 0 ? null : this.row(target) })
    }
    return { rows, total: end - start, offset }
  }

  private referrerPage(index: number, offset: number, limit: number): EdgePage {
    const store = this.requireStore()
    this.checkIndex(index)
    this.ensureGraph()
    const all = referrers(this.preds!, index)
    return {
      rows: all.slice(offset, offset + limit).map((r) => ({ name: store.edgeNameText(r.edge), item: this.row(r.source) })),
      total: all.length,
      offset,
    }
  }

  private paths(index: number, max: number): ResponseMap['paths'] {
    const store = this.requireStore()
    this.checkIndex(index)
    this.ensureGraph()
    this.finder ??= new ReferencePathFinder(store, this.preds!, this.ignored)
    const { paths, rootsReaching } = this.finder.find(index, max)
    return {
      paths: paths.map((path) => ({
        self: path.self,
        steps: path.steps.map((step) => ({ name: step.edge < 0 ? null : store.edgeNameText(step.edge), item: this.row(step.item) })),
      })),
      rootsReaching,
    }
  }

  private graph(request: Extract<Request, { kind: 'graph' }>): ResponseMap['graph'] {
    const store = this.requireStore()
    this.checkIndex(request.index)
    this.ensureGraph()
    const maxNodes = Math.max(1, Math.min(request.maxNodes, MAX_GRAPH_NODES))
    const depth = Math.max(1, Math.min(request.depth, 5))

    let subgraph
    if (request.mode === 'paths') {
      this.finder ??= new ReferencePathFinder(store, this.preds!, this.ignored)
      subgraph = pathsSubgraph(this.finder, this.dominators, request.index, maxNodes)
    } else {
      subgraph = neighbourhoodSubgraph(store, this.preds!, request.index, depth, maxNodes, request.direction)
    }

    const bottlenecks = new Set(subgraph.bottlenecks)
    const dominators = this.dominators
    log.debug('graph built', { mode: request.mode, nodes: subgraph.nodes.length, edges: subgraph.edges.length, truncated: subgraph.truncated })

    return {
      target: request.index,
      mode: request.mode,
      nodes: subgraph.nodes.map((index) => ({
        ...this.row(index),
        isTarget: index === request.index,
        isShared: dominators !== null && dominators.idom[index] === dominators.virtualRoot && !store.isRoot(index),
        isBottleneck: bottlenecks.has(index),
      })),
      edges: subgraph.edges.map((edge) => ({ source: edge.source, target: edge.target, name: store.edgeNameText(edge.edge) })),
      truncated: subgraph.truncated,
      totalRoots: subgraph.totalRoots,
    }
  }

  private row(index: number): ItemRow {
    const store = this.requireStore()
    const view = store.item(index)
    const row: ItemRow = {
      index,
      address: view.address,
      type: view.type,
      size: view.size,
      isRoot: view.isRoot,
      children: store.childStart[index + 1]! - store.childStart[index]!,
    }
    if (view.className !== undefined) row.className = view.className
    if (view.symbol !== undefined) row.symbol = view.symbol
    if (view.frame !== undefined) row.frame = view.frame
    if (view.extras?.declaredAt !== undefined) row.declaredAt = view.extras.declaredAt
    if (this.dominators !== null && this.dominators.idom[index] !== -1) row.retained = this.dominators.retained[index]!
    return row
  }

  private ensureGraph(): void {
    const store = this.requireStore()
    this.preds ??= buildPredecessors(store)
    this.distances ??= rootDistances(store, this.ignored)
  }

  private requireStore(): DumpStore {
    if (this.store === null) throw new NoDumpError()
    return this.store
  }

  private checkIndex(index: number): void {
    if (!Number.isInteger(index) || index < 0 || index >= this.requireStore().count) {
      throw new InvalidQueryError(`no item ${index}`)
    }
  }
}
