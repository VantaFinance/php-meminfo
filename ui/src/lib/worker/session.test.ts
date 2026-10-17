import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import type { Phase } from './protocol'
import { DumpSession, toWorkerError } from './session'

const file = (name: string): File =>
  new File([readFileSync(new URL(`../../../tests/fixtures/${name}.json`, import.meta.url))], `${name.split('/').pop()}.json`)

const noProgress = (): void => {}

describe('DumpSession', () => {
  it('loads, analyzes and answers every request', async () => {
    const session = new DumpSession()
    const phases: Phase[] = []

    const overview = await session.handle({ kind: 'load', file: file('parity/leak-after') }, (phase) => phases.push(phase))
    expect(overview).toMatchObject({ fileName: 'leak-after.json', stats: { items: 1300 } })
    expect(overview.roots).toBeGreaterThan(0)
    expect(phases).toContain('resolving')

    const analysis = await session.handle({ kind: 'analyze' }, noProgress)
    expect(analysis.reachable).toBe(1300)

    const suspects = await session.handle({ kind: 'suspects', limit: 10 }, noProgress)
    const top = suspects.suspects[0]!
    expect(top.symbol).toBe('LeakyCache::requests')
    expect(top.retained).toBeGreaterThan(0)

    const detail = await session.handle({ kind: 'item', index: top.index }, noProgress)
    expect(detail).toMatchObject({ distanceFromRoot: 0, shared: false })
    expect(detail.dominated).toBeGreaterThan(200)
    expect(detail.dominatorChain.at(-1)?.index).toBe(top.index)

    const children = await session.handle({ kind: 'children', index: top.index, offset: 10, limit: 5 }, noProgress)
    expect(children).toMatchObject({ total: 200, offset: 10 })
    expect(children.rows.map((row) => row.name)).toEqual(['10', '11', '12', '13', '14'])
    const request = children.rows[0]!.item!
    expect(request.className).toBe('Request')

    const paths = await session.handle({ kind: 'paths', index: request.index, max: 10 }, noProgress)
    expect(paths.rootsReaching).toBe(1)
    expect(paths.paths[0]?.steps.map((step) => step.name)).toEqual([null, '10'])

    const referrers = await session.handle({ kind: 'referrers', index: request.index, offset: 0, limit: 10 }, noProgress)
    expect(referrers.rows.map((row) => row.item?.symbol)).toContain('LeakyCache::requests')

    const page = await session.handle(
      { kind: 'query', filters: [{ attribute: 'class', operator: '=', value: 'Request' }], offset: 0, limit: 3 },
      noProgress,
    )
    expect(page.total).toBe(200)
    expect(page.rows).toHaveLength(3)

    const found = await session.handle({ kind: 'find', address: request.address.toUpperCase() }, noProgress)
    expect(found).toBe(request.index)

    const summary = await session.handle({ kind: 'summary' }, noProgress)
    expect(summary.find((row) => row.key === 'Request')?.count).toBe(200)
  })

  it('builds bounded graphs of paths and neighbourhoods', async () => {
    const session = new DumpSession()
    await session.handle({ kind: 'load', file: file('parity/leak-after') }, noProgress)
    await session.handle({ kind: 'analyze' }, noProgress)
    const page = await session.handle({ kind: 'query', filters: [{ attribute: 'class', operator: '=', value: 'Request' }], offset: 0, limit: 1 }, noProgress)
    const request = page.rows[0]!.index

    const paths = await session.handle({ kind: 'graph', index: request, mode: 'paths', depth: 1, direction: 'both', maxNodes: 300 }, noProgress)
    expect(paths.nodes.map((node) => node.symbol ?? node.className)).toEqual(['Request', 'LeakyCache::requests'])
    expect(paths.nodes.find((node) => node.isTarget)?.index).toBe(request)
    expect(paths.nodes.find((node) => node.isBottleneck)?.symbol).toBe('LeakyCache::requests')
    expect(paths.edges).toHaveLength(1)
    expect(paths.totalRoots).toBe(1)

    const around = await session.handle({ kind: 'graph', index: request, mode: 'neighbourhood', depth: 1, direction: 'both', maxNodes: 300 }, noProgress)
    expect(around.nodes.length).toBe(1 + 4) // the request, its 3 children and the cache array
    expect(around.edges.map((edge) => edge.name).sort()).toEqual(['0', 'id', 'payload', 'tags'])

    const capped = await session.handle({ kind: 'graph', index: request, mode: 'neighbourhood', depth: 3, direction: 'both', maxNodes: 5_000_000 }, noProgress)
    expect(capped.nodes.length).toBeLessThanOrEqual(1000)
  })

  it('compares with a baseline reduced in another session', async () => {
    const baseline = await new DumpSession().handle({ kind: 'loadBaseline', file: file('parity/leak-before') }, noProgress)
    const session = new DumpSession()
    await session.handle({ kind: 'load', file: file('parity/leak-after') }, noProgress)

    const growth = await session.handle({ kind: 'diff', baseline }, noProgress)

    expect(growth.find((row) => row.key === 'Request')?.countDelta).toBe(150)
  })

  it('maps failures to error kinds', async () => {
    const session = new DumpSession()

    await expect(session.handle({ kind: 'summary' }, noProgress)).rejects.toSatisfy((e) => toWorkerError(e).kind === 'no-dump')

    const broken = new File(['{"items":{"0x1":{"type":'], 'broken.json')
    await expect(session.handle({ kind: 'load', file: broken }, noProgress)).rejects.toSatisfy((e) => toWorkerError(e).kind === 'invalid-dump')

    await session.handle({ kind: 'load', file: file('parity/basic') }, noProgress)
    await expect(
      session.handle({ kind: 'query', filters: [{ attribute: 'class', operator: '~', value: '(' }], offset: 0, limit: 1 }, noProgress),
    ).rejects.toSatisfy((e) => toWorkerError(e).kind === 'invalid-query')
    await expect(session.handle({ kind: 'item', index: 10_000_000 }, noProgress)).rejects.toSatisfy((e) => toWorkerError(e).kind === 'invalid-query')
  })
})
