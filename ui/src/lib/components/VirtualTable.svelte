<script lang="ts" module>
  export interface Column<T> {
    key: string
    label: string
    /** CSS grid track, e.g. "1fr" or "120px". */
    width: string
    align?: 'left' | 'right'
    value: (row: T) => string
    /** Enables sorting on this column (in-memory rows only). */
    compare?: (a: T, b: T) => number
    title?: (row: T) => string
  }
</script>

<script lang="ts" generics="T">
  import { untrack, type Snippet } from 'svelte'
  import { createLogger } from '../log'

  /**
   * Windowed table: only the visible rows exist in the DOM. Rows come either
   * from `rows` (small in-memory lists) or page by page from `load` (results
   * held by the worker), with a bounded page cache.
   *
   * Browsers cap element heights (~33.5M px in Chromium/WebKit, Firefox breaks
   * below 20M), so past MAX_SPACER the scroll position is mapped to a row index
   * proportionally instead of 1:1.
   */
  interface Props {
    label: string
    columns: Column<T>[]
    total?: number
    rows?: T[]
    load?: (offset: number, limit: number) => Promise<T[]>
    /** Change it to drop cached pages (a new query). */
    resetKey?: unknown
    selectedKey?: unknown
    rowKey?: (row: T) => unknown
    onActivate?: (row: T) => void
    cell?: Snippet<[T, Column<T>]>
    empty?: Snippet
    height?: string
  }

  let {
    label,
    columns,
    total,
    rows,
    load,
    resetKey,
    selectedKey,
    rowKey,
    onActivate,
    cell,
    empty,
    height = '100%',
  }: Props = $props()

  const log = createLogger('table')
  const id = `table-${Math.random().toString(36).slice(2, 9)}`
  const ROW_HEIGHT = 30
  const MAX_SPACER = 8_000_000
  const PAGE = 100
  const MAX_PAGES = 30

  let viewport: HTMLDivElement | undefined = $state()
  let scrollTop = $state(0)
  let clientHeight = $state(0)
  let activeIndex = $state(0)
  let sortKey = $state<string | null>(null)
  let sortDescending = $state(true)

  // Page cache for `load`: a plain Map (not reactive) plus a version counter.
  let pages = new Map<number, T[]>()
  let inFlight = new Set<number>()
  let version = $state(0)

  $effect(() => {
    void resetKey
    void load
    // untrack: reading `version` or `viewport` here would make every loaded page
    // re-run this reset and empty the cache again.
    untrack(() => {
      pages = new Map()
      inFlight = new Set()
      version++
      activeIndex = 0
      if (viewport) viewport.scrollTop = 0
    })
  })

  const sorted = $derived.by(() => {
    if (rows === undefined) return undefined
    const column = columns.find((c) => c.key === sortKey)
    if (column?.compare === undefined) return rows
    const compare = column.compare
    const copy = [...rows].sort(compare)
    return sortDescending ? copy.reverse() : copy
  })

  const count = $derived(sorted?.length ?? total ?? 0)
  const scaled = $derived(count * ROW_HEIGHT > MAX_SPACER)
  const spacer = $derived(scaled ? MAX_SPACER : count * ROW_HEIGHT)
  const visible = $derived(Math.ceil(clientHeight / ROW_HEIGHT) + 1)
  const first = $derived.by(() => {
    if (!scaled) return Math.min(Math.floor(scrollTop / ROW_HEIGHT), Math.max(0, count - 1))
    const range = Math.max(1, spacer - clientHeight)
    return Math.min(Math.floor((scrollTop / range) * Math.max(0, count - visible + 1)), Math.max(0, count - visible))
  })
  const windowTop = $derived(scaled ? scrollTop : first * ROW_HEIGHT)
  const indices = $derived.by(() => {
    const out: number[] = []
    for (let i = first; i < Math.min(count, first + visible); i++) out.push(i)
    return out
  })
  const template = $derived(columns.map((c) => c.width).join(' '))
  // Short in-memory lists shrink to their rows; `height` is then a maximum.
  const fitted = $derived(rows !== undefined && count > 0 ? `min(${height}, ${32 + count * ROW_HEIGHT + 2}px)` : height)

  function rowAt(index: number): T | undefined {
    void version
    if (sorted !== undefined) return sorted[index]
    return pages.get(Math.floor(index / PAGE))?.[index % PAGE]
  }

  // Fetch the pages the window needs.
  $effect(() => {
    if (load === undefined || count === 0) return
    const loader = load
    const firstPage = Math.floor(first / PAGE)
    const lastPage = Math.floor((first + visible) / PAGE)
    for (let page = firstPage; page <= lastPage; page++) {
      if (pages.has(page) || inFlight.has(page) || page * PAGE >= count) continue
      inFlight.add(page)
      const cache = pages
      log.debug('page requested', { page })
      loader(page * PAGE, PAGE)
        .then((result) => {
          if (cache !== pages) return // reset meanwhile
          pages.set(page, result)
          while (pages.size > MAX_PAGES) pages.delete(pages.keys().next().value!)
          version++
        })
        .catch((error: unknown) => log.warn('page failed', { page, error: String(error) }))
        .finally(() => inFlight.delete(page))
    }
  })

  let frame = 0
  function onScroll(): void {
    if (frame !== 0) return
    frame = requestAnimationFrame(() => {
      frame = 0
      if (viewport) scrollTop = viewport.scrollTop
    })
  }

  $effect(() => () => {
    if (frame !== 0) cancelAnimationFrame(frame)
  })

  function scrollToIndex(index: number): void {
    if (!viewport) return
    if (scaled) {
      const range = Math.max(1, spacer - clientHeight)
      viewport.scrollTop = (index / Math.max(1, count - visible + 1)) * range
      return
    }
    const top = index * ROW_HEIGHT
    if (top < viewport.scrollTop) viewport.scrollTop = top
    else if (top + ROW_HEIGHT > viewport.scrollTop + clientHeight) viewport.scrollTop = top + ROW_HEIGHT - clientHeight
  }

  function activate(index: number): void {
    activeIndex = index
    const row = rowAt(index)
    if (row !== undefined) onActivate?.(row)
  }

  function onKeydown(event: KeyboardEvent): void {
    if (count === 0) return
    let next = activeIndex
    if (event.key === 'ArrowDown') next = Math.min(count - 1, activeIndex + 1)
    else if (event.key === 'ArrowUp') next = Math.max(0, activeIndex - 1)
    else if (event.key === 'PageDown') next = Math.min(count - 1, activeIndex + visible - 1)
    else if (event.key === 'PageUp') next = Math.max(0, activeIndex - visible + 1)
    else if (event.key === 'Home') next = 0
    else if (event.key === 'End') next = count - 1
    else if (event.key === 'Enter') {
      activate(activeIndex)
      event.preventDefault()
      return
    } else return
    event.preventDefault()
    activeIndex = next
    scrollToIndex(next)
  }

  function sortBy(column: Column<T>): void {
    if (column.compare === undefined || rows === undefined) return
    if (sortKey === column.key) sortDescending = !sortDescending
    else {
      sortKey = column.key
      sortDescending = true
    }
  }
</script>

{#snippet header()}
  <div class="header" role="row" aria-rowindex={1} style:grid-template-columns={template}>
    {#each columns as column (column.key)}
      <div
        class="th"
        class:right={column.align === 'right'}
        role="columnheader"
        aria-sort={sortKey === column.key ? (sortDescending ? 'descending' : 'ascending') : undefined}
      >
        {#if column.compare !== undefined && rows !== undefined}
          <button type="button" class="sort" onclick={() => sortBy(column)}>
            {column.label}
            {#if sortKey === column.key}<span aria-hidden="true">{sortDescending ? '↓' : '↑'}</span>{/if}
          </button>
        {:else}
          {column.label}
        {/if}
      </div>
    {/each}
  </div>
{/snippet}

<div class="table" style:height={fitted}>
  {#if count === 0}
    <div role="table" aria-label={label}>
      {@render header()}
      <div class="empty">
        {#if empty}{@render empty()}{:else}Nothing to show.{/if}
      </div>
    </div>
  {:else}
    <!-- The grid holds the keyboard focus and points at the active row with
         aria-activedescendant, so the rows stay out of the tab order. -->
    <div
      class="viewport"
      bind:this={viewport}
      bind:clientHeight
      onscroll={onScroll}
      onkeydown={onKeydown}
      tabindex="0"
      role="grid"
      aria-label={label}
      aria-rowcount={count + 1}
      aria-activedescendant={`${id}-row-${activeIndex}`}
    >
      {@render header()}
      <div class="spacer" role="rowgroup" style:height="{spacer}px">
        <div class="window" style:transform="translateY({windowTop}px)">
          {#each indices as index (index)}
            {@const row = rowAt(index)}
            <div
              id={`${id}-row-${index}`}
              tabindex="-1"
              class="tr"
              class:active={index === activeIndex}
              class:selected={row !== undefined && rowKey !== undefined && selectedKey !== undefined && rowKey(row) === selectedKey}
              role="row"
              aria-rowindex={index + 2}
              style:grid-template-columns={template}
              onclick={() => activate(index)}
              onkeydown={onKeydown}
            >
              {#if row === undefined}
                {#each columns as column (column.key)}
                  <div class="td" role="gridcell"><span class="skeleton placeholder"></span></div>
                {/each}
              {:else}
                {#each columns as column (column.key)}
                  <div class="td" class:right={column.align === 'right'} role="gridcell" title={column.title?.(row) ?? column.value(row)}>
                    {#if cell}{@render cell(row, column)}{:else}{column.value(row)}{/if}
                  </div>
                {/each}
              {/if}
            </div>
          {/each}
        </div>
      </div>
    </div>
  {/if}
</div>

<style>
  .table {
    display: flex;
    flex-direction: column;
    min-height: 0;
    border: 1px solid var(--border);
    border-radius: var(--radius-md);
    background: var(--surface-raised);
    overflow: hidden;
  }

  .header,
  .tr {
    display: grid;
    align-items: center;
    column-gap: var(--space-3);
    padding: 0 var(--space-3);
  }

  .header {
    position: sticky;
    top: 0;
    z-index: 1;
    flex: none;
    height: 32px;
    border-bottom: 1px solid var(--border);
    background: var(--surface-sunken);
    color: var(--text-muted);
    font-size: var(--text-xs);
    font-weight: 600;
    letter-spacing: 0.02em;
    text-transform: uppercase;
  }

  .th {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .sort {
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    letter-spacing: inherit;
    text-transform: inherit;
    cursor: pointer;
  }

  .sort:hover {
    color: var(--text);
  }

  .right {
    text-align: right;
    justify-content: flex-end;
  }

  .viewport {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    position: relative;
    outline-offset: -2px;
  }

  .spacer {
    position: relative;
  }

  .window {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    will-change: transform;
  }

  .tr {
    height: 30px;
    border-bottom: 1px solid var(--border);
    font-size: var(--text-sm);
    cursor: pointer;
    transition: background var(--motion);
  }

  .tr:hover {
    background: var(--surface-hover);
  }

  .tr.active {
    box-shadow: inset 2px 0 0 var(--border-strong);
  }

  .viewport:focus-visible .tr.active {
    box-shadow: inset 2px 0 0 var(--focus);
  }

  .tr.selected {
    background: var(--accent-soft);
  }

  .td {
    display: flex;
    align-items: center;
    gap: var(--space-2);
    min-width: 0;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }

  .placeholder {
    display: block;
    width: 70%;
    height: 10px;
  }

  .empty {
    padding: var(--space-5);
    color: var(--text-muted);
    font-size: var(--text-sm);
  }
</style>
