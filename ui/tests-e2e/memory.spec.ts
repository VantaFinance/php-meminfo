import { expect, test, type Page } from '@playwright/test'
import { DUMPS, dumpPath } from './global-setup'
import { mainHeapMb, workerCount, workerHeaps } from './helpers'

/**
 * The page must not leak, nor freeze, on a large dump. Chromium only: the
 * probes are CDP.
 */

/** Main-thread JS heap allowed above the empty page while a large dump is open. */
const MAIN_HEAP_BUDGET_MB = 60
/** What a closed dump may leave behind on the main thread. */
const RETAINED_AFTER_CLOSE_MB = 15

async function openLarge(page: Page, name: keyof typeof DUMPS = 'large'): Promise<number> {
  const started = Date.now()
  await page.locator('input[type=file]').first().setInputFiles(dumpPath(name))
  await expect(page.getByRole('heading', { name: 'Leak suspects' })).toBeVisible({ timeout: 120_000 })
  await expect(page.getByRole('grid', { name: 'Biggest retainers' }).getByRole('row').nth(1)).toContainText('LeakyCache::requests')
  return Date.now() - started
}

test('a ~300 MB dump stays in the worker and the page stays responsive', async ({ page, browser }) => {
  await page.goto('/')
  const emptyHeap = await mainHeapMb(page)

  // Responsiveness while parsing: the main thread answers within 200 ms.
  await page.locator('input[type=file]').first().setInputFiles(dumpPath('large'))
  const latencies: number[] = []
  for (let i = 0; i < 5; i++) {
    const started = Date.now()
    await page.evaluate(() => 1 + 1)
    latencies.push(Date.now() - started)
    if (!(await page.getByRole('progressbar').isVisible())) break
  }
  expect(Math.max(...latencies)).toBeLessThan(200)
  await expect(page.getByRole('grid', { name: 'Biggest retainers' }).getByRole('row').nth(1)).toContainText('LeakyCache::requests', { timeout: 120_000 })

  const mainHeap = await mainHeapMb(page)
  const workers = await workerHeaps(browser)
  console.info('[meminfo-ui:e2e] open', { emptyHeap, mainHeap, workers })

  expect(mainHeap - emptyHeap).toBeLessThan(MAIN_HEAP_BUDGET_MB)
  expect(workers).toHaveLength(1)
  // The store lives in the worker's typed arrays, outside its JS heap.
  expect(workers[0]!.backingStorageMb).toBeGreaterThan(50)
  expect(workers[0]!.usedMb).toBeLessThan(400)
})

test('three open/close cycles leave nothing behind', async ({ page, browser }) => {
  let closed = 0
  page.on('worker', (worker) => worker.once('close', () => closed++))
  await page.goto('/')
  const emptyHeap = await mainHeapMb(page)

  const timings: number[] = []
  for (let cycle = 0; cycle < 3; cycle++) {
    timings.push(await openLarge(page))
    await page.getByRole('button', { name: 'Close' }).click()
    await expect(page.getByRole('button', { name: 'Open dump…' })).toBeVisible()
    await expect.poll(() => workerCount(browser)).toBe(0)
  }

  const heap = await mainHeapMb(page)
  console.info('[meminfo-ui:e2e] cycles', { emptyHeap, heap, timings })
  expect(closed).toBe(3)
  expect(heap - emptyHeap).toBeLessThan(RETAINED_AFTER_CLOSE_MB)
  // ~300 MB parse + analysis in Chrome takes about a second; keep headroom for CI.
  expect(Math.max(...timings)).toBeLessThan(30_000)
})

test('cancelling mid-parse frees the worker', async ({ page, browser }) => {
  await page.goto('/')
  await page.locator('input[type=file]').first().setInputFiles(dumpPath('large'))
  await page.getByRole('button', { name: 'Cancel' }).click()

  await expect(page.getByRole('button', { name: 'Open dump…' })).toBeVisible()
  await expect.poll(() => workerCount(browser)).toBe(0)
})

test('a dump past the 512 MiB string limit still opens', async ({ page }) => {
  test.skip(process.env.MEMINFO_E2E_HUGE !== '1', 'opt-in: MEMINFO_E2E_HUGE=1 (generates ~650 MB)')
  await page.goto('/')
  await openLarge(page, 'huge')
})
