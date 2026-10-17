import { readFileSync } from 'node:fs'
import cytoscape from 'cytoscape'
import { describe, expect, it } from 'vitest'
import { DumpSession } from '../../worker/session'
import type { GraphResult } from '../../worker/protocol'
import { graphStyle, indexOfNodeId, layoutOptions, nodeId, toElements } from './elements'

const tokens = {
  text: '#111', muted: '#666', surface: '#fff', border: '#ccc', accent: '#25d', accentSoft: '#eef', warning: '#a60', danger: '#c22', font: 'sans-serif',
}

async function graphOf(mode: 'paths' | 'neighbourhood'): Promise<GraphResult> {
  const session = new DumpSession()
  const bytes = readFileSync(new URL('../../../../tests/fixtures/parity/leak-after.json', import.meta.url))
  await session.handle({ kind: 'load', file: new File([bytes], 'leak-after.json') }, () => {})
  await session.handle({ kind: 'analyze' }, () => {})
  // A string inside a Request's tags: a path of four hops from the static array.
  const tags = await session.handle({ kind: 'query', filters: [{ attribute: 'class', operator: '=', value: 'Request' }], offset: 0, limit: 1 }, () => {})
  const request = tags.rows[0]!.index
  const children = await session.handle({ kind: 'children', index: request, offset: 0, limit: 10 }, () => {})
  const tagsArray = children.rows.find((row) => row.name === 'tags')!.item!.index
  const tag = (await session.handle({ kind: 'children', index: tagsArray, offset: 0, limit: 1 }, () => {})).rows[0]!.item!.index
  return session.handle({ kind: 'graph', index: tag, mode, depth: 2, direction: 'both', maxNodes: 300 }, () => {})
}

describe('graph elements', () => {
  it('lays merged paths out from the roots on the left to the target on the right', async () => {
    const result = await graphOf('paths')
    const cy = cytoscape({ headless: true, elements: toElements(result), style: graphStyle(tokens) })
    cy.layout(layoutOptions(result)).run()

    const x = (index: number): number => cy.getElementById(nodeId(index)).position('x')
    const target = result.nodes.find((node) => node.isTarget)!
    for (const node of result.nodes.filter((n) => n.isRoot)) {
      expect(x(node.index)).toBeLessThan(x(target.index))
    }
    expect(cy.nodes()).toHaveLength(result.nodes.length)
    expect(cy.edges()).toHaveLength(result.edges.length)
    expect(cy.$('node.target')).toHaveLength(1)
    expect(cy.$('node.bottleneck').length).toBeGreaterThan(0)
    expect(cy.$('node.root').data('label')).toContain('LeakyCache::requests')

    cy.destroy()
    expect(cy.destroyed()).toBe(true)
  })

  it('puts holders left of the item and what it holds right of it, in a neighbourhood', async () => {
    const result = await graphOf('neighbourhood')
    const cy = cytoscape({ headless: true, elements: toElements(result) })
    cy.layout(layoutOptions(result)).run()

    const x = (index: number): number => cy.getElementById(nodeId(index)).position('x')
    for (const edge of result.edges) {
      expect(x(edge.source)).toBeLessThan(x(edge.target))
    }
    expect(cy.nodes().length).toBe(result.nodes.length)
    cy.destroy()
  })

  it('round-trips node ids', () => {
    expect(indexOfNodeId(nodeId(1234))).toBe(1234)
  })
})
