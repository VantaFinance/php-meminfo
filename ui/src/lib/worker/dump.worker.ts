/// <reference lib="webworker" />
import { createLogger, setLogLevel } from '../log'
import type { FromWorker, ToWorker } from './protocol'
import { DumpSession, toWorkerError } from './session'

declare const self: DedicatedWorkerGlobalScope

/**
 * One dump per worker. The page terminates the worker to cancel a load or to
 * drop a dump: the only way to be sure everything it allocated is released.
 */

const log = createLogger('worker')
const session = new DumpSession()

// Requests are handled one at a time, in order: a query sent while the dump is
// still loading waits for it.
let queue: Promise<void> = Promise.resolve()

function post(message: FromWorker): void {
  self.postMessage(message)
}

self.onmessage = (event: MessageEvent<ToWorker>) => {
  const message = event.data
  if (message.type === 'init') {
    setLogLevel(message.logLevel)
    log.debug('worker ready')
    return
  }

  const { id, request } = message
  queue = queue.then(async () => {
    const started = performance.now()
    try {
      const value = await session.handle(request, (phase, bytes, totalBytes, items) =>
        post({ type: 'progress', id, phase, bytes, totalBytes, items }),
      )
      post({ type: 'result', id, ok: true, value })
      log.debug('request done', { kind: request.kind, ms: Math.round(performance.now() - started) })
    } catch (error) {
      const workerError = toWorkerError(error)
      log.warn('request failed', { kind: request.kind, error: workerError.kind, message: workerError.message })
      post({ type: 'result', id, ok: false, error: workerError })
    }
  })
}
