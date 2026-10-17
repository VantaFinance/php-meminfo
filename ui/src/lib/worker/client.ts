import { createLogger, getLogLevel } from '../log'
import type { Baseline } from '../engine/analyses/diff'
import type { FromWorker, Phase, Request, ResponseMap, ToWorker, WorkerError } from './protocol'

const log = createLogger('client')

export class WorkerFailure extends Error {
  readonly kind: WorkerError['kind']

  constructor(error: WorkerError) {
    super(error.message)
    this.name = 'WorkerFailure'
    this.kind = error.kind
  }
}

export type ProgressListener = (phase: Phase, bytes: number, totalBytes: number, items: number) => void

interface Pending {
  resolve: (value: unknown) => void
  reject: (error: Error) => void
  onProgress?: ProgressListener
}

/**
 * The page's handle on one dump worker. dispose() terminates the worker and
 * drops every reference to it, which releases the whole dump.
 */
export class DumpClient {
  private worker: Worker | null
  private readonly pending = new Map<number, Pending>()
  private nextId = 1
  private deathListener: ((error: WorkerFailure) => void) | null = null

  constructor() {
    this.worker = new Worker(new URL('./dump.worker.ts', import.meta.url), { type: 'module' })
    this.worker.onmessage = (event: MessageEvent<FromWorker>) => this.receive(event.data)
    // A worker that dies without answering — most often out of memory — rejects
    // everything in flight instead of leaving the page waiting.
    this.worker.onerror = (event) => {
      event.preventDefault()
      this.die(`the analysis worker stopped: ${event.message || 'unknown error'}`)
    }
    this.worker.onmessageerror = () => this.die('a message from the analysis worker could not be read')
    this.post({ type: 'init', logLevel: getLogLevel() })
    log.debug('worker spawned')
  }

  /** Called once if the worker dies on its own. */
  onDeath(listener: (error: WorkerFailure) => void): void {
    this.deathListener = listener
  }

  request<K extends Request['kind']>(request: Extract<Request, { kind: K }>, onProgress?: ProgressListener): Promise<ResponseMap[K]> {
    if (this.worker === null) {
      return Promise.reject(new WorkerFailure({ kind: 'cancelled', message: 'the dump was closed' }))
    }
    const id = this.nextId++
    const started = performance.now()
    return new Promise<ResponseMap[K]>((resolve, reject) => {
      const pending: Pending = {
        resolve: (value) => {
          log.debug('response', { kind: request.kind, ms: Math.round(performance.now() - started) })
          resolve(value as ResponseMap[K])
        },
        reject,
      }
      if (onProgress !== undefined) pending.onProgress = onProgress
      this.pending.set(id, pending)
      this.post({ type: 'request', id, request })
    })
  }

  load(file: File, onProgress?: ProgressListener): Promise<ResponseMap['load']> {
    return this.request({ kind: 'load', file }, onProgress)
  }

  get alive(): boolean {
    return this.worker !== null
  }

  /** Terminate the worker: cancels whatever it was doing and frees the dump. */
  dispose(): void {
    if (this.worker === null) return
    this.worker.onmessage = null
    this.worker.onerror = null
    this.worker.onmessageerror = null
    this.worker.terminate()
    this.worker = null
    this.deathListener = null
    for (const pending of this.pending.values()) {
      pending.reject(new WorkerFailure({ kind: 'cancelled', message: 'the dump was closed' }))
    }
    this.pending.clear()
    log.debug('worker terminated')
  }

  private post(message: ToWorker): void {
    this.worker?.postMessage(message)
  }

  private receive(message: FromWorker): void {
    const pending = this.pending.get(message.id)
    if (pending === undefined) return
    if (message.type === 'progress') {
      pending.onProgress?.(message.phase, message.bytes, message.totalBytes, message.items)
      return
    }
    this.pending.delete(message.id)
    if (message.ok) {
      pending.resolve(message.value)
    } else {
      pending.reject(new WorkerFailure(message.error))
    }
  }

  private die(message: string): void {
    log.warn('worker died', { message })
    const failure = new WorkerFailure({ kind: 'worker-died', message })
    const listener = this.deathListener
    for (const pending of this.pending.values()) pending.reject(failure)
    this.pending.clear()
    this.dispose()
    listener?.(failure)
  }
}

/**
 * Parse a baseline dump in a worker of its own, keep only its aggregates and
 * terminate it: comparing two dumps never holds two stores.
 */
export async function loadBaseline(file: File, onProgress?: ProgressListener, signal?: AbortSignal): Promise<Baseline> {
  const client = new DumpClient()
  const abort = (): void => client.dispose()
  signal?.addEventListener('abort', abort, { once: true })
  try {
    return await client.request({ kind: 'loadBaseline', file }, onProgress)
  } finally {
    signal?.removeEventListener('abort', abort)
    client.dispose()
  }
}
