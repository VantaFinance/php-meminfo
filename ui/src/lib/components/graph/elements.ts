import type { BreadthFirstLayoutOptions, ElementDefinition, StylesheetJson } from 'cytoscape'
import { formatBytes } from '../../format'
import type { GraphNode, GraphResult } from '../../worker/protocol'

/**
 * GraphResult → Cytoscape elements, stylesheet and layout. Pure: no DOM, no
 * Cytoscape instance, so it is unit-tested headless in Node.
 */

export const nodeId = (index: number): string => `n${index}`

export function indexOfNodeId(id: string): number {
  return Number(id.slice(1))
}

function nodeClasses(node: GraphNode): string {
  const classes: string[] = []
  if (node.isTarget) classes.push('target')
  if (node.isRoot) classes.push('root')
  if (node.isShared) classes.push('shared')
  if (node.isBottleneck) classes.push('bottleneck')
  return classes.join(' ')
}

function nodeLabel(node: GraphNode): string {
  const what = node.className ?? node.type
  const size = node.retained === undefined ? '' : `\n${formatBytes(node.retained)}`
  return node.symbol === undefined ? `${what}${size}` : `${what}\n${node.symbol}${size}`
}

const FONT_SIZE = 11
const CHAR_WIDTH = 6.6
const LINE_HEIGHT = 14
const PADDING = 10

/**
 * Box that fits the label. Cytoscape's own `width: 'label'` is deprecated, so
 * the size is worked out here from the label's lines.
 */
function nodeSize(label: string): { w: number; h: number } {
  const lines = label.split('\n')
  const longest = Math.max(...lines.map((line) => line.length))
  return { w: Math.round(longest * CHAR_WIDTH + 2 * PADDING), h: lines.length * LINE_HEIGHT + 2 * PADDING - 4 }
}

export function toElements(result: GraphResult): ElementDefinition[] {
  const elements: ElementDefinition[] = result.nodes.map((node) => {
    const label = nodeLabel(node)
    return {
      group: 'nodes',
      data: { id: nodeId(node.index), label, ...nodeSize(label) },
      classes: nodeClasses(node),
    }
  })
  result.edges.forEach((edge, i) => {
    elements.push({
      group: 'edges',
      data: { id: `e${i}`, source: nodeId(edge.source), target: nodeId(edge.target), label: edge.name },
    })
  })
  return elements
}

/**
 * Layered left to right, following the references: the layers start at the
 * nodes nothing in the picture points to (the roots of merged paths, or the
 * parents in a neighbourhood), so holders stand left of what they hold.
 * `maximal` pushes every node to its deepest layer, so the target of merged
 * paths of different lengths still ends up rightmost. A picture that is all
 * cycle falls back to the target.
 */
export function layoutOptions(result: GraphResult): BreadthFirstLayoutOptions {
  const pointedAt = new Set(result.edges.map((edge) => edge.target))
  let roots = result.nodes.filter((node) => !pointedAt.has(node.index)).map((node) => nodeId(node.index))
  if (roots.length === 0) roots = [nodeId(result.target)]
  return {
    name: 'breadthfirst',
    roots,
    directed: true,
    direction: 'rightward',
    maximal: true,
    spacingFactor: 1.3,
    avoidOverlap: true,
    nodeDimensionsIncludeLabels: true,
    padding: 24,
    animate: false,
  }
}

/** The CSS custom properties the graph is drawn with. */
export interface GraphTokens {
  text: string
  muted: string
  surface: string
  border: string
  accent: string
  accentSoft: string
  warning: string
  danger: string
  font: string
}

export function graphStyle(tokens: GraphTokens): StylesheetJson {
  return [
    {
      selector: 'node',
      style: {
        shape: 'round-rectangle',
        width: 'data(w)',
        height: 'data(h)',
        'background-color': tokens.surface,
        'border-width': 1,
        'border-color': tokens.border,
        label: 'data(label)',
        color: tokens.text,
        'font-family': tokens.font,
        'font-size': FONT_SIZE,
        'text-wrap': 'wrap',
        'text-valign': 'center',
        'text-halign': 'center',
      },
    },
    { selector: 'node.root', style: { 'border-color': tokens.accent, 'border-width': 2 } },
    { selector: 'node.shared', style: { 'border-color': tokens.warning, 'border-style': 'dashed', 'border-width': 2 } },
    { selector: 'node.bottleneck', style: { 'background-color': tokens.accentSoft, 'border-color': tokens.accent, 'border-width': 3 } },
    { selector: 'node.target', style: { 'border-color': tokens.danger, 'border-width': 3 } },
    { selector: 'node:selected', style: { 'overlay-color': tokens.accent, 'overlay-opacity': 0.15 } },
    {
      selector: 'edge',
      style: {
        width: 1.5,
        'line-color': tokens.border,
        'target-arrow-color': tokens.border,
        'target-arrow-shape': 'triangle',
        'curve-style': 'bezier',
        label: 'data(label)',
        'font-family': tokens.font,
        'font-size': 10,
        color: tokens.muted,
        'text-background-color': tokens.surface,
        'text-background-opacity': 1,
        'text-background-padding': '2px',
      },
    },
  ]
}
