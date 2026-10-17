import { createLogger } from '../log'
import type { QueryFilter } from '../engine/analyses/basic'
import type { Baseline } from '../engine/analyses/diff'
import { DumpClient, loadBaseline, WorkerFailure } from '../worker/client'
import type { Analysis, Overview, Phase } from '../worker/protocol'

/**
 * Application state.
 *
 * Every value that comes from the worker is held in $state.raw: replaced, never
 * mutated, never wrapped in proxies. The dump itself never reaches the page —
 * only the DumpClient handle, which is not reactive state at all.
 */

const log = createLogger('app')

/** Chrome holds about 15M items (~2.5 GB of dump) within a 2 GB worker budget. */
export const LARGE_DUMP_BYTES = 2.5 * 1024 ** 3

const LOADING_MARKER = 'meminfo-ui:loading'

export type View = 'suspects' | 'growth' | 'summary' | 'top' | 'search' | 'graph'

export interface LoadProgress {
  phase: Phase
  bytes: number
  totalBytes: number
  items: number
}

export interface LoadError {
  kind: string
  message: string
  fileName: string
}

/** What a load that crashed the tab left behind in sessionStorage. */
export interface CrashReport {
  fileName: string
  fileSize: number
  phase: string
}

type Status = 'empty' | 'loading' | 'ready' | 'error' | 'crashed'

let client: DumpClient | null = null
let baselineAbort: (() => void) | null = null

function readMarker(): CrashReport | null {
  try {
    const text = sessionStorage.getItem(LOADING_MARKER)
    return text === null ? null : (JSON.parse(text) as CrashReport)
  } catch {
    return null
  }
}

function writeMarker(report: CrashReport | null): void {
  try {
    if (report === null) sessionStorage.removeItem(LOADING_MARKER)
    else sessionStorage.setItem(LOADING_MARKER, JSON.stringify(report))
  } catch {
    // Storage unavailable: crash detection degrades to the worker's own death.
  }
}

class AppState {
  status = $state<Status>('empty')
  view = $state<View>('suspects')
  overview = $state.raw<Overview | null>(null)
  analysis = $state.raw<Analysis | null>(null)
  progress = $state.raw<LoadProgress | null>(null)
  error = $state.raw<LoadError | null>(null)
  crash = $state.raw<CrashReport | null>(null)
  pendingLarge = $state.raw<File | null>(null)

  baseline = $state.raw<Baseline | null>(null)
  baselineProgress = $state.raw<LoadProgress | null>(null)
  baselineError = $state<string | null>(null)

  /** Filters the search view starts from (set when another view links to it). */
  searchFilters = $state.raw<QueryFilter[]>([])

  /** Item the graph view is centred on. */
  graphTarget = $state<number | null>(null)

  /** Item shown in the inspector, and the way back. */
  selected = $state<number | null>(null)
  history = $state.raw<number[]>([])

  /** Bumped when a new dump is ready, so views refetch. */
  generation = $state(0)

  constructor() {
    // A marker left by a load that never finished: the tab most likely crashed
    // (a worker out of JS heap takes the whole tab down in Chromium).
    const marker = readMarker()
    if (marker !== null) {
      this.crash = marker
      this.status = 'crashed'
      writeMarker(null)
      log.error('the previous load did not finish', { ...marker })
    }
  }

  get client(): DumpClient | null {
    return client
  }

  /** Entry point for a picked or dropped file. */
  open(file: File, force = false): void {
    if (!force && file.size > LARGE_DUMP_BYTES) {
      this.pendingLarge = file
      return
    }
    this.pendingLarge = null
    void this.load(file)
  }

  private async load(file: File): Promise<void> {
    this.close()
    this.status = 'loading'
    this.error = null
    this.crash = null
    this.progress = { phase: 'reading', bytes: 0, totalBytes: file.size, items: 0 }
    const started = performance.now()
    log.info('load started', { size: file.size })

    const own = new DumpClient()
    client = own
    own.onDeath((failure) => this.workerDied(file, failure))
    writeMarker({ fileName: file.name, fileSize: file.size, phase: 'reading' })

    try {
      const overview = await own.load(file, (phase, bytes, totalBytes, items) => {
        this.progress = { phase, bytes, totalBytes, items }
      })
      if (client !== own) return
      this.overview = overview
      writeMarker({ fileName: file.name, fileSize: file.size, phase: 'analyzing' })
      this.progress = { phase: 'analyzing', bytes: file.size, totalBytes: file.size, items: overview.stats.items }

      const analysis = await own.request({ kind: 'analyze' })
      if (client !== own) return
      this.analysis = analysis
      this.status = 'ready'
      this.view = 'suspects'
      this.generation++
      this.progress = null
      writeMarker(null)
      log.info('load finished', { items: overview.stats.items, ms: Math.round(performance.now() - started) })
    } catch (error) {
      if (client !== own) return
      writeMarker(null)
      const failure = error instanceof WorkerFailure ? error : new WorkerFailure({ kind: 'internal', message: String(error) })
      if (failure.kind === 'cancelled') {
        log.warn('load cancelled')
        this.status = 'empty'
        return
      }
      if (failure.kind === 'worker-died') return // handled by workerDied
      this.error = { kind: failure.kind, message: failure.message, fileName: file.name }
      this.status = 'error'
      this.dropClient()
    }
  }

  private workerDied(file: File, failure: WorkerFailure): void {
    log.error('analysis worker died', { size: file.size, phase: this.progress?.phase ?? 'ready', message: failure.message })
    writeMarker(null)
    this.crash = { fileName: file.name, fileSize: file.size, phase: this.progress?.phase ?? 'browsing' }
    this.status = 'crashed'
    client = null
    this.resetDumpState()
  }

  cancel(): void {
    log.warn('load cancelled by the user')
    writeMarker(null)
    this.close()
    this.status = 'empty'
  }

  /** Drop the dump: terminating the worker frees everything it held. */
  close(): void {
    this.dropClient()
    this.resetDumpState()
    this.status = 'empty'
  }

  dismiss(): void {
    this.crash = null
    this.error = null
    this.status = 'empty'
  }

  private dropClient(): void {
    client?.dispose()
    client = null
  }

  private resetDumpState(): void {
    baselineAbort?.()
    this.overview = null
    this.analysis = null
    this.progress = null
    this.baseline = null
    this.baselineProgress = null
    this.baselineError = null
    this.selected = null
    this.history = []
    this.searchFilters = []
    // Indices mean nothing in another dump: a graph must not outlive its dump.
    this.graphTarget = null
  }

  async openBaseline(file: File): Promise<void> {
    baselineAbort?.()
    this.baselineError = null
    this.baselineProgress = { phase: 'reading', bytes: 0, totalBytes: file.size, items: 0 }
    const controller = new AbortController()
    baselineAbort = () => controller.abort()
    const aborted = (): boolean => controller.signal.aborted
    try {
      const baseline = await loadBaseline(
        file,
        (phase, bytes, totalBytes, items) => {
          if (!aborted()) this.baselineProgress = { phase, bytes, totalBytes, items }
        },
        controller.signal,
      )
      if (aborted()) return
      this.baseline = baseline
      this.view = 'growth'
      log.info('baseline loaded', { items: baseline.items })
    } catch (error) {
      if (!aborted()) this.baselineError = error instanceof Error ? error.message : String(error)
    } finally {
      if (!aborted()) this.baselineProgress = null
    }
  }

  clearBaseline(): void {
    baselineAbort?.()
    this.baseline = null
    this.baselineProgress = null
    this.baselineError = null
  }

  /** Open the graph view on an item. */
  openGraph(index: number): void {
    log.debug('graph opened', { index })
    this.graphTarget = index
    this.view = 'graph'
  }

  /** Open the search view on the instances of a type or class. */
  searchFor(key: string, isClass: boolean): void {
    this.searchFilters = [{ attribute: isClass ? 'class' : 'type', operator: '=', value: key }]
    this.view = 'search'
  }

  select(index: number | null): void {
    if (index === this.selected) return
    if (this.selected !== null && index !== null) this.history = [...this.history.slice(-49), this.selected]
    if (index === null) this.history = []
    this.selected = index
  }

  back(): void {
    const previous = this.history.at(-1)
    if (previous === undefined) return
    this.history = this.history.slice(0, -1)
    this.selected = previous
  }
}

export const app = new AppState()
