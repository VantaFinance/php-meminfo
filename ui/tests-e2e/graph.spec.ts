import { expect, test, type Page } from '@playwright/test'
import { fileURLToPath } from 'node:url'
import { dumpPath } from './global-setup'
import { mainHeapMb, watchCsp } from './helpers'

const fixture = (name: string): string => fileURLToPath(new URL(`../tests/fixtures/${name}.json`, import.meta.url))

/** Same budget as memory.spec.ts: main-thread heap above the empty page with a large dump open. */
const MAIN_HEAP_BUDGET_MB = 60
/** What five graph open/close cycles may add on top of the first one. */
const GROWTH_OVER_CYCLES_MB = 10

interface Node {
  x: number
  y: number
  label: string
}

/**
 * Nodes of the drawn graph, in page coordinates. Cytoscape keeps its instance
 * on the container (`_cyreg`), which is how the test finds where to click.
 */
async function nodes(page: Page, selector: string): Promise<Node[]> {
  const canvas = page.getByRole('img', { name: /^Graph of/ })
  const box = (await canvas.boundingBox())!
  const found = await canvas.evaluate((element, wanted) => {
    const cy = (element as unknown as { _cyreg: { cy: import('cytoscape').Core } })._cyreg.cy
    return cy.nodes(wanted).map((node) => ({ ...node.renderedPosition(), label: String(node.data('label')) }))
  }, selector)
  return found.map((node) => ({ x: box.x + node.x, y: box.y + node.y, label: node.label }))
}

/** Nodes once the picture has stopped moving (a resize re-fits it). */
async function settledNodes(page: Page, selector: string): Promise<Node[]> {
  let previous = ''
  let current: Node[] = []
  await expect
    .poll(async () => {
      current = await nodes(page, selector)
      const key = JSON.stringify(current)
      const same = key === previous
      previous = key
      return same && current.length > 0
    }, { intervals: [100] })
    .toBe(true)
  return current
}

async function openGraphFromSearch(page: Page): Promise<void> {
  await page.getByRole('button', { name: /^Search/ }).click()
  await page.getByLabel('Value').first().fill('Request')
  await page.getByRole('button', { name: 'Search', exact: true }).click()
  await expect(page.getByText('200 items match')).toBeVisible()
  await page.getByRole('grid', { name: 'Search results' }).locator('.address-link').first().click()
}

test('draws what keeps an item reachable, from an address link', async ({ page, context, baseURL }) => {
  const origin = new URL(baseURL!).origin
  const external: string[] = []
  const requested: string[] = []
  context.on('request', (request) => {
    const url = request.url()
    requested.push(url)
    if (url.startsWith('blob:') || url.startsWith('data:')) return
    if (new URL(url).origin !== origin) external.push(url)
  })
  await page.route((url) => url.origin !== origin && url.protocol.startsWith('http'), (route) => route.abort())
  const cspViolations = await watchCsp(page)
  const cytoscapeRequests = (): number => requested.filter((url) => url.includes('cytoscape')).length

  await page.goto('/')
  await page.locator('input[type=file]').first().setInputFiles(fixture('parity/leak-after'))
  await expect(page.getByRole('grid', { name: 'Biggest retainers' }).getByRole('row').nth(1)).toContainText('LeakyCache::requests')
  expect(cytoscapeRequests()).toBe(0)

  // An address in the search results opens the graph of that item.
  await openGraphFromSearch(page)
  const summary = page.getByTestId('graph-summary')
  await expect(summary).toHaveText(/2 items · 1 references · 1 root reaches Request/)
  expect(cytoscapeRequests()).toBe(1)

  // The static array is the bottleneck: every path goes through it.
  await expect.poll(async () => (await nodes(page, 'node.bottleneck')).map((node) => node.label)).toEqual([expect.stringContaining('LeakyCache::requests')])

  // A tap selects the node in the inspector.
  const [bottleneck] = await settledNodes(page, 'node.bottleneck')
  await page.mouse.click(bottleneck!.x, bottleneck!.y)
  await expect(page.locator('.inspector h2')).toHaveText('array')

  // A double tap brings in the neighbours of the node.
  const [target] = await settledNodes(page, 'node.target')
  await page.mouse.dblclick(target!.x, target!.y)
  await expect.poll(async () => Number((await summary.textContent())!.match(/^\s*(\d+) items/)![1])).toBeGreaterThan(2)

  // Neighbourhood: the Request, the array holding it and its three properties.
  await page.getByRole('button', { name: 'Neighbourhood' }).click()
  await expect(summary).toHaveText(/5 items · 4 references/)
  await expect.poll(async () => (await nodes(page, 'node')).length).toBe(5)

  expect(cytoscapeRequests()).toBe(1)
  expect(external).toEqual([])
  expect(await cspViolations()).toEqual([])
})

test('the inspector opens the graph of the selected item', async ({ page }) => {
  await page.goto('/')
  await page.locator('input[type=file]').first().setInputFiles(fixture('parity/leak-after'))
  await page.getByRole('grid', { name: 'Biggest retainers' }).getByRole('row').nth(1).click()
  await page.getByRole('button', { name: 'Show graph' }).click()
  await expect(page.getByTestId('graph-summary')).toContainText('1 root reaches array')
})

test('opening and closing the graph on a ~300 MB dump leaves nothing behind', async ({ page }) => {
  await page.goto('/')
  const emptyHeap = await mainHeapMb(page)
  await page.locator('input[type=file]').first().setInputFiles(dumpPath('large'))
  const retainers = page.getByRole('grid', { name: 'Biggest retainers' })
  await expect(retainers.getByRole('row').nth(1)).toContainText('LeakyCache::requests', { timeout: 120_000 })
  await retainers.getByRole('row').nth(1).click()

  const heaps: number[] = []
  for (let cycle = 0; cycle < 5; cycle++) {
    await page.getByRole('button', { name: 'Show graph' }).click()
    await expect(page.getByTestId('graph-summary')).toBeVisible()
    await expect.poll(async () => (await nodes(page, 'node')).length).toBeGreaterThan(0)
    await page.getByRole('button', { name: /^Leak suspects/ }).click()
    await expect(page.getByRole('img', { name: /^Graph of/ })).toHaveCount(0)
    await retainers.getByRole('row').nth(1).click()
    heaps.push(await mainHeapMb(page))
  }

  console.info('[meminfo-ui:e2e] graph cycles', { emptyHeap, heaps })
  expect(Math.max(...heaps) - emptyHeap).toBeLessThan(MAIN_HEAP_BUDGET_MB)
  expect(heaps.at(-1)! - heaps[0]!).toBeLessThan(GROWTH_OVER_CYCLES_MB)
})
