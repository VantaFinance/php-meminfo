import { expect, test } from '@playwright/test'
import { watchCsp } from './helpers'

test('the start page explains the extension and its settings without leaving the page', async ({ page, context, baseURL }) => {
  const origin = new URL(baseURL!).origin
  const external: string[] = []
  context.on('request', (request) => {
    const url = request.url()
    if (url.startsWith('blob:') || url.startsWith('data:')) return
    if (new URL(url).origin !== origin) external.push(url)
  })
  const cspViolations = await watchCsp(page)

  await page.setViewportSize({ width: 1280, height: 720 })
  await page.goto('/')

  await expect(page).toHaveTitle('PHP Meminfo — memory dump analyzer')
  await expect(page.locator('meta[name="description"]')).toHaveAttribute('content', /never uploaded/)

  // The drop zone stays the first thing on screen.
  const open = page.getByRole('button', { name: 'Open dump…' })
  await expect(open).toBeInViewport()

  for (const heading of ['What it is', 'Install the extension', 'Settings', 'Getting a dump']) {
    await expect(page.getByRole('heading', { name: heading, level: 2 })).toBeVisible()
  }

  const settings = page.getByRole('table')
  await expect(settings.getByRole('row', { name: /meminfo\.dump_signal/ })).toContainText('SIGURG')
  await expect(settings.getByRole('row', { name: /meminfo\.dump_dir/ })).toContainText('system temp dir')

  // Details are links to the README, not content fetched into the page.
  const links = page.locator('.guide a')
  expect(await links.count()).toBeGreaterThan(0)
  for (const href of await links.evaluateAll((anchors) => anchors.map((a) => a.getAttribute('href')))) {
    expect(href).toMatch(/^https:\/\/github\.com\/VantaFinance\/php-meminfo(#[a-z-]+)?$/)
  }

  expect(external).toEqual([])
  expect(await cspViolations()).toEqual([])
})

test('the start page fits a phone screen', async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 800 })
  await page.goto('/')

  await expect(page.getByRole('button', { name: 'Open dump…' })).toBeInViewport()
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth)
  expect(overflow).toBe(0)
})
