import { expect, test } from '@playwright/test'
import { fileURLToPath } from 'node:url'
import { watchCsp } from './helpers'

const fixture = (name: string): string => fileURLToPath(new URL(`../tests/fixtures/${name}.json`, import.meta.url))

test('finds the planted leak, its growth and its reference path, without leaving the page', async ({ page, context, baseURL }) => {
  const origin = new URL(baseURL!).origin
  const external: string[] = []
  context.on('request', (request) => {
    const url = request.url()
    if (url.startsWith('blob:') || url.startsWith('data:')) return
    if (new URL(url).origin !== origin) external.push(url)
  })
  // Nothing may leave: abort anything that tries, and fail the test below.
  await page.route((url) => url.origin !== origin && url.protocol.startsWith('http'), (route) => route.abort())
  page.on('websocket', (socket) => external.push(socket.url()))
  const cspViolations = await watchCsp(page)

  await page.goto('/')
  await expect(page.getByRole('heading', { name: 'PHP Meminfo' })).toBeVisible()
  await page.locator('input[type=file]').first().setInputFiles(fixture('parity/leak-after'))

  // Leak suspects: the static array holding every Request comes first.
  const retainers = page.getByRole('grid', { name: 'Biggest retainers' })
  await expect(retainers.getByRole('row').nth(1)).toContainText('LeakyCache::requests')
  await expect(page.getByText(/may be missing its other holders/)).toBeVisible()

  // Inspector: kept alive by, children, and a clickable reference path.
  await retainers.getByRole('row').nth(1).click()
  await expect(page.getByRole('heading', { name: 'Reference paths from roots' })).toBeVisible()
  const children = page.getByRole('grid', { name: 'Children' })
  await expect(children.getByRole('row').nth(1)).toContainText('Request')
  await children.getByRole('row').nth(1).click()
  await expect(page.locator('.paths li').first()).toContainText('LeakyCache::requests')
  await page.locator('.paths li button').first().click()
  await expect(page.locator('.inspector h2')).toHaveText('array')

  // Growth against the earlier dump: Request grew by 150.
  await page.getByRole('button', { name: /Growth/ }).click()
  await page.locator('input[type=file]').last().setInputFiles(fixture('parity/leak-before'))
  const growth = page.getByRole('grid', { name: 'Growth since the baseline' })
  await expect(growth.getByRole('row').filter({ hasText: 'Request' })).toContainText('+150')

  // Summary links into the search.
  await page.getByRole('button', { name: /Summary/ }).click()
  await page.getByRole('grid', { name: 'Summary by type and class' }).getByRole('row').filter({ hasText: 'Request' }).click()
  await expect(page.getByText('200 items match')).toBeVisible()

  expect(external).toEqual([])
  expect(await cspViolations()).toEqual([])
})

test('opens a dump json_decode rejects (invalid UTF-8 in keys)', async ({ page }) => {
  await page.goto('/')
  await page.locator('input[type=file]').first().setInputFiles(fixture('ui-only/invalid-utf8'))
  await expect(page.getByRole('heading', { name: 'Leak suspects' })).toBeVisible()
})

test('reports a malformed dump instead of failing silently', async ({ page }) => {
  await page.goto('/')
  await page.locator('input[type=file]').first().setInputFiles({ name: 'broken.json', mimeType: 'application/json', buffer: Buffer.from('{"items":{"0x10":{"type":') })
  await expect(page.getByRole('alert')).toContainText('broken.json could not be opened')
})

test('shows the recovery screen after a load that never finished', async ({ page }) => {
  await page.goto('/')
  // What a crashed tab leaves behind: the marker of a load in progress.
  await page.evaluate(() => sessionStorage.setItem('meminfo-ui:loading', JSON.stringify({ fileName: 'big.json', fileSize: 3e9, phase: 'reading' })))
  await page.reload()

  await expect(page.getByRole('heading', { name: 'The browser ran out of memory' })).toBeVisible()
  await expect(page.getByText('big.json')).toBeVisible()
  await page.getByRole('button', { name: 'Open another dump' }).click()
  await expect(page.getByRole('button', { name: 'Open dump…' })).toBeVisible()
})
