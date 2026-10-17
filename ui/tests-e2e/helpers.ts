import type { Browser, Page } from '@playwright/test'

/**
 * Memory probes over the Chrome DevTools Protocol.
 *
 * The page's JS heap (Performance.getMetrics) does not include its workers, and
 * a worker's typed arrays are not in its JS heap either: they show up only as
 * `backingStorageSize` of Runtime.getHeapUsage on the worker's own target.
 */

const MB = 1024 * 1024

export async function mainHeapMb(page: Page): Promise<number> {
  const cdp = await page.context().newCDPSession(page)
  await cdp.send('HeapProfiler.collectGarbage')
  await cdp.send('Performance.enable')
  const { metrics } = await cdp.send('Performance.getMetrics')
  await cdp.detach()
  const used = metrics.find((metric) => metric.name === 'JSHeapUsedSize')?.value ?? 0
  return used / MB
}

export interface WorkerHeap {
  url: string
  usedMb: number
  backingStorageMb: number
}

/** Heap of every dedicated worker the browser runs, after a GC in each. */
export async function workerHeaps(browser: Browser): Promise<WorkerHeap[]> {
  const cdp = await browser.newBrowserCDPSession()
  const { targetInfos } = await cdp.send('Target.getTargets')
  const workers = targetInfos.filter((target) => target.type === 'worker')
  const heaps: WorkerHeap[] = []

  for (const worker of workers) {
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId: worker.targetId, flatten: false })
    let nextId = 1
    const call = (method: string): Promise<Record<string, number>> =>
      new Promise((resolve, reject) => {
        const id = nextId++
        const onMessage = (event: { sessionId?: string; message: string }): void => {
          if (event.sessionId !== sessionId) return
          const message = JSON.parse(event.message) as { id?: number; result?: Record<string, number>; error?: { message: string } }
          if (message.id !== id) return
          cdp.off('Target.receivedMessageFromTarget', onMessage)
          if (message.error) reject(new Error(message.error.message))
          else resolve(message.result ?? {})
        }
        cdp.on('Target.receivedMessageFromTarget', onMessage)
        void cdp.send('Target.sendMessageToTarget', { sessionId, message: JSON.stringify({ id, method }) })
      })

    await call('HeapProfiler.collectGarbage')
    const usage = await call('Runtime.getHeapUsage')
    heaps.push({
      url: worker.url,
      usedMb: (usage.usedSize ?? 0) / MB,
      backingStorageMb: (usage.backingStorageSize ?? 0) / MB,
    })
    await cdp.send('Target.detachFromTarget', { sessionId })
  }

  await cdp.detach()
  return heaps
}

export async function workerCount(browser: Browser): Promise<number> {
  const cdp = await browser.newBrowserCDPSession()
  const { targetInfos } = await cdp.send('Target.getTargets')
  await cdp.detach()
  return targetInfos.filter((target) => target.type === 'worker').length
}

/** Records CSP violations from the very first script on. */
export async function watchCsp(page: Page): Promise<() => Promise<string[]>> {
  await page.addInitScript(() => {
    const violations: string[] = []
    ;(window as unknown as { __csp: string[] }).__csp = violations
    document.addEventListener('securitypolicyviolation', (event) => violations.push(`${event.effectiveDirective} ${event.blockedURI}`))
  })
  return () => page.evaluate(() => (window as unknown as { __csp: string[] }).__csp)
}
