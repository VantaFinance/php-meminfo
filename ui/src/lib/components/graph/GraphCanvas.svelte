<script lang="ts">
  import type { Core } from 'cytoscape'
  import { createLogger } from '../../log'
  import type { GraphResult } from '../../worker/protocol'
  import { graphStyle, indexOfNodeId, layoutOptions, nodeId, toElements, type GraphTokens } from './elements'

  /**
   * One Cytoscape instance per mounted canvas.
   *
   * Cytoscape is ~140 KB gzip — more than the rest of the app — so it is loaded
   * on first use, as its own chunk. The instance is destroyed when the canvas
   * goes away (cy.destroy() stops its render loop and drops its references);
   * if the canvas goes away before the chunk has even loaded, no instance is
   * created at all.
   */
  interface Props {
    result: GraphResult
    selected: number | null
    onSelect: (index: number) => void
    onExpand: (index: number) => void
    onError: (message: string) => void
  }

  let { result, selected, onSelect, onExpand, onError }: Props = $props()

  const log = createLogger('graph')

  let container: HTMLDivElement | undefined = $state()
  let cy = $state.raw<Core | null>(null)

  function readTokens(): GraphTokens {
    const css = getComputedStyle(document.documentElement)
    const value = (name: string): string => css.getPropertyValue(name).trim()
    return {
      text: value('--text'),
      muted: value('--text-muted'),
      surface: value('--surface-raised'),
      border: value('--border-strong'),
      accent: value('--accent'),
      accentSoft: value('--accent-soft'),
      warning: value('--warning'),
      danger: value('--danger'),
      font: value('--font-sans'),
    }
  }

  // Create once, destroy on teardown.
  $effect(() => {
    const element = container
    if (element === undefined) return
    let cancelled = false
    let instance: Core | null = null
    const started = performance.now()

    import('cytoscape')
      .then(({ default: cytoscape }) => {
        if (cancelled) return
        instance = cytoscape({
          container: element,
          style: graphStyle(readTokens()),
          minZoom: 0.1,
          maxZoom: 3,
          boxSelectionEnabled: false,
          selectionType: 'single',
        })
        // onetap, not tap: selecting opens the inspector, which narrows the
        // canvas under the pointer, so the second half of a double tap would
        // miss the node. onetap fires only once no second tap followed.
        instance.on('onetap', 'node', (event) => onSelect(indexOfNodeId(event.target.id())))
        instance.on('dbltap', 'node', (event) => onExpand(indexOfNodeId(event.target.id())))
        cy = instance
        log.debug('cytoscape ready', { ms: Math.round(performance.now() - started) })
      })
      .catch((error: unknown) => {
        log.warn('cytoscape could not be loaded', { error: String(error) })
        onError('The graph library could not be loaded.')
      })

    // Colours follow the light/dark scheme.
    const scheme = matchMedia('(prefers-color-scheme: dark)')
    const restyle = (): void => {
      instance?.style(graphStyle(readTokens()))
    }
    scheme.addEventListener('change', restyle)

    // Cytoscape only follows window resizes; the inspector opening or closing
    // resizes the container alone.
    const resized = new ResizeObserver(() => {
      if (instance === null) return
      instance.resize()
      fitCapped(instance)
    })
    resized.observe(element)

    return () => {
      cancelled = true
      resized.disconnect()
      scheme.removeEventListener('change', restyle)
      if (instance !== null) {
        instance.destroy()
        log.debug('cytoscape destroyed')
      }
      instance = null
      cy = null
    }
  })

  // Draw the result whenever it changes.
  $effect(() => {
    const instance = cy
    const graph = result
    if (instance === null) return
    const started = performance.now()
    instance.batch(() => {
      instance.elements().remove()
      instance.add(toElements(graph))
    })
    instance.layout(layoutOptions(graph)).run()
    fitCapped(instance)
    log.debug('graph laid out', { nodes: graph.nodes.length, edges: graph.edges.length, ms: Math.round(performance.now() - started) })
  })

  // Mirror the inspector's selection.
  $effect(() => {
    const instance = cy
    const index = selected
    void result
    if (instance === null) return
    instance.$(':selected').unselect()
    if (index !== null) instance.getElementById(nodeId(index)).select()
  })

  /** Fit, but never blow a small graph up past its natural size. */
  function fitCapped(instance: Core): void {
    instance.fit(undefined, 24)
    if (instance.zoom() > 1) {
      instance.zoom(1)
      instance.center()
    }
  }

  export function fit(): void {
    if (cy !== null) fitCapped(cy)
  }
</script>

<div class="canvas" bind:this={container} role="img" aria-label={`Graph of ${result.nodes.length} items and ${result.edges.length} references`}>
  {#if cy === null}
    <div class="loading skeleton" aria-hidden="true"></div>
  {/if}
</div>

<style>
  .canvas {
    position: relative;
    width: 100%;
    height: 100%;
    min-height: 320px;
    border: 1px solid var(--border);
    border-radius: var(--radius-md);
    background: var(--surface-raised);
    overflow: hidden;
  }

  .loading {
    position: absolute;
    inset: var(--space-4);
  }
</style>
