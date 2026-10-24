<script lang="ts">
  import { formatBytes, formatCount, formatPercent } from '../format'
  import { createLogger } from '../log'
  import { app } from '../state/app.svelte'
  import type { EdgePage, ItemDetail, ItemRow, PathsResult } from '../worker/protocol'
  import AddressLink from './AddressLink.svelte'
  import ItemLabel from './ItemLabel.svelte'
  import VirtualTable, { type Column } from './VirtualTable.svelte'

  interface Props {
    index: number
  }

  let { index }: Props = $props()

  const log = createLogger('inspector')

  let detail = $state.raw<ItemDetail | null>(null)
  let paths = $state.raw<PathsResult | null>(null)
  let failed = $state<string | null>(null)

  $effect(() => {
    const client = app.client
    const target = index
    detail = null
    paths = null
    failed = null
    if (client === null) return
    let cancelled = false
    Promise.all([client.request({ kind: 'item', index: target }), client.request({ kind: 'paths', index: target, max: 20 })])
      .then(([itemDetail, pathResult]) => {
        if (cancelled) return
        if (itemDetail.row.type === 'resource' && itemDetail.extras?.resourceType === undefined) {
          log.debug('resource of an unknown type (or an older dump)', { index: target })
        }
        detail = itemDetail
        paths = pathResult
      })
      .catch((error: unknown) => {
        if (cancelled) return
        failed = String(error)
        log.warn('item failed', { error: failed })
      })
    return () => {
      cancelled = true
    }
  })

  function edgeLoader(kind: 'children' | 'referrers'): (offset: number, limit: number) => Promise<EdgePage['rows']> {
    return async (offset, limit) => {
      const client = app.client
      if (client === null) return []
      const page = await client.request({ kind, index, offset, limit })
      return page.rows
    }
  }

  const loadChildren = $derived(edgeLoader('children'))
  const loadReferrers = $derived(edgeLoader('referrers'))

  type Edge = EdgePage['rows'][number]
  const edgeColumns: Column<Edge>[] = [
    { key: 'name', label: 'Key', width: 'minmax(70px, 0.5fr)', value: (row) => row.name },
    { key: 'address', label: 'Address', width: '120px', value: (row) => row.item?.address ?? '' },
    { key: 'item', label: 'Item', width: 'minmax(120px, 1fr)', value: (row) => (row.item === null ? 'missing from the dump' : `${row.item.className ?? row.item.type}`) },
    { key: 'retained', label: 'Retained', width: '84px', align: 'right', value: (row) => (row.item?.retained === undefined ? '—' : formatBytes(row.item.retained)) },
  ]

  const total = $derived(app.analysis !== null && app.overview !== null ? app.overview.totalSize : 0)

  // "Open in PhpStorm" links for declarations: off by default, remembered per
  // browser. They only hand a local path to the IDE; nothing leaves the page.
  const EDITOR_LINKS_KEY = 'meminfoEditorLinks'

  function readEditorLinks(): boolean {
    try {
      return localStorage.getItem(EDITOR_LINKS_KEY) === '1'
    } catch {
      return false
    }
  }

  let editorLinks = $state(readEditorLinks())

  function setEditorLinks(enabled: boolean): void {
    editorLinks = enabled
    try {
      localStorage.setItem(EDITOR_LINKS_KEY, enabled ? '1' : '0')
    } catch {
      // Storage unavailable (private window): the choice lasts for this page only.
    }
  }

  /** "file:line" → a phpstorm:// URL, or null unless it ends in a line number (it comes from the dump). */
  function editorUrl(location: string): string | null {
    const colon = location.lastIndexOf(':')
    const line = location.slice(colon + 1)
    if (colon <= 0 || !/^\d+$/.test(line)) return null
    return `phpstorm://open?file=${encodeURIComponent(location.slice(0, colon))}&line=${line}`
  }
</script>

{#snippet itemLink(row: ItemRow, name: string | null)}
  <button type="button" class="link" onclick={() => app.select(row.index)}>
    {#if name !== null}<span class="edge mono">[{name}]</span>{/if}
    <ItemLabel {row} />
  </button>
{/snippet}

<aside class="inspector" aria-label="Item details">
  <header class="bar">
    <button type="button" class="button quiet" onclick={() => app.back()} disabled={app.history.length === 0}>← Back</button>
    <button type="button" class="button quiet" aria-label="Close details" onclick={() => app.select(null)}>✕</button>
  </header>

  {#if failed}
    <div class="notice danger" role="alert">{failed}</div>
  {:else if detail === null}
    <div class="loading">
      <div class="skeleton" style:height="22px" style:width="60%"></div>
      <div class="skeleton" style:height="80px"></div>
      <div class="skeleton" style:height="140px"></div>
    </div>
  {:else}
    {@const row = detail.row}
    <div class="title">
      <h2>{row.className ?? row.type}</h2>
      <div class="tags">
        {#if row.isRoot}<span class="badge root">root</span>{/if}
        {#if detail.shared}<span class="badge shared" title="Held by several roots, retained by none of them alone">shared</span>{/if}
      </div>
    </div>
    <p class="address"><AddressLink index={row.index} address={row.address} /></p>
    {#if row.symbol}<p class="symbol"><span class="mono">{row.symbol}</span>{#if row.frame}{' '}<span class="muted">in {row.frame}</span>{/if}</p>{/if}

    <dl class="facts">
      <div><dt>Self size</dt><dd>{formatBytes(row.size)}</dd></div>
      <div>
        <dt>Retained</dt>
        <dd>{row.retained === undefined ? '—' : formatBytes(row.retained)} <span class="muted">{row.retained === undefined ? '' : formatPercent(row.retained, total)}</span></dd>
      </div>
      <div><dt>Items kept alive</dt><dd>{detail.dominated === undefined ? '—' : formatCount(detail.dominated)}</dd></div>
      <div><dt>Distance from a root</dt><dd>{detail.distanceFromRoot < 0 ? 'unreachable' : formatCount(detail.distanceFromRoot)}</dd></div>
      <div><dt>Children</dt><dd>{formatCount(row.children)}</dd></div>
      <div><dt>Referenced by</dt><dd>{formatCount(detail.referrers)}</dd></div>
      {#if detail.objectHandle !== undefined}<div><dt>Object handle</dt><dd>#{detail.objectHandle}</dd></div>{/if}
      {#if detail.extras?.line !== undefined}<div><dt>Frame at line</dt><dd>{detail.extras.line}</dd></div>{/if}
      {#if detail.extras?.function}<div><dt>Function</dt><dd class="mono">{detail.extras.function}</dd></div>{/if}
      {#if detail.extras?.declaredAt}
        {@const url = editorLinks ? editorUrl(detail.extras.declaredAt) : null}
        <div>
          <dt>Declared at</dt>
          <dd class="mono">{#if url}<a href={url}>{detail.extras.declaredAt}</a>{:else}{detail.extras.declaredAt}{/if}</dd>
        </div>
      {/if}
      {#if detail.extras?.resourceType}<div><dt>Resource</dt><dd>{detail.extras.resourceType}</dd></div>{/if}
      {#if detail.extras?.capacity !== undefined}
        <div>
          <dt>Slots used</dt>
          <dd>{formatCount(detail.extras.used ?? 0)} of {formatCount(detail.extras.capacity)}</dd>
        </div>
      {/if}
    </dl>
    {#if detail.extras?.declaredAt}
      <label class="hint">
        <input type="checkbox" checked={editorLinks} onchange={(event) => setEditorLinks(event.currentTarget.checked)} />
        Link declarations to PhpStorm
      </label>
    {/if}

    {#if detail.dominatorChain.length > 1}
      <section>
        <h3>Kept alive by</h3>
        <p class="hint">Each item here alone keeps the next one in memory.</p>
        <ol class="chain">
          {#each detail.dominatorChain as link (link.index)}
            <li>
              {#if link.index === row.index}
                <ItemLabel row={link} />
              {:else}
                {@render itemLink(link, null)}
              {/if}
              {#if link.retained !== undefined}<span class="muted num">{formatBytes(link.retained)}</span>{/if}
              {#if link.index !== row.index}
                <button
                  type="button"
                  class="button quiet"
                  title="Recompute as if no {link.className ?? link.type} held anything"
                  onclick={() => app.ignoreHolder(link.className ?? link.type)}>Ignore</button
                >
              {/if}
            </li>
          {/each}
        </ol>
      </section>
    {/if}

    {#if paths}
      <section>
        <div class="section-head">
          <h3>Reference paths from roots</h3>
          <button type="button" class="button quiet" onclick={() => app.openGraph(row.index)}>Show graph</button>
        {#if row.className}
          <button type="button" class="button quiet" onclick={() => row.className && app.showRetainers({ key: row.className, isClass: true })}>
            How all {row.className} are held
          </button>
        {/if}
        </div>
        {#if paths.paths.length === 0}
          <p class="hint">No root reaches this item.</p>
        {:else}
          <p class="hint">
            {formatCount(paths.rootsReaching)} {paths.rootsReaching === 1 ? 'root reaches' : 'roots reach'} it{#if paths.rootsReaching > paths.paths.length}; the {paths.paths.length} nearest are shown{/if}.
          </p>
          <ul class="paths">
            {#each paths.paths as path, i (i)}
              <li>
                {#if path.self}
                  <span class="muted">It is a root itself</span>
                {:else}
                  {#each path.steps as step, s (s)}
                    {#if s > 0}<span class="arrow" aria-hidden="true">→</span>{/if}
                    {#if step.item.index === row.index}
                      <span class="edge mono">[{step.name}]</span>
                    {:else}
                      {@render itemLink(step.item, step.name)}
                    {/if}
                  {/each}
                {/if}
              </li>
            {/each}
          </ul>
        {/if}
      </section>
    {/if}

    {#if row.children > 0}
      <section>
        <h3>Children</h3>
        <VirtualTable
          label="Children"
          columns={edgeColumns}
          total={row.children}
          load={loadChildren}
          resetKey={index}
          onActivate={(edge) => edge.item && app.select(edge.item.index)}
          height="240px"
        >
          {#snippet cell(edge, column)}
            {#if column.key === 'name'}<span class="mono">{edge.name}</span>{:else if column.key === 'address' && edge.item}<AddressLink index={edge.item.index} address={edge.item.address} />{:else if column.key === 'item' && edge.item}<ItemLabel row={edge.item} />{:else}{column.value(edge)}{/if}
          {/snippet}
        </VirtualTable>
      </section>
    {/if}

    {#if detail.referrers > 0}
      <section>
        <h3>Referenced by</h3>
        <VirtualTable
          label="Referenced by"
          columns={edgeColumns}
          total={detail.referrers}
          load={loadReferrers}
          resetKey={index}
          onActivate={(edge) => edge.item && app.select(edge.item.index)}
          height="200px"
        >
          {#snippet cell(edge, column)}
            {#if column.key === 'name'}<span class="mono">{edge.name}</span>{:else if column.key === 'address' && edge.item}<AddressLink index={edge.item.index} address={edge.item.address} />{:else if column.key === 'item' && edge.item}<ItemLabel row={edge.item} />{:else}{column.value(edge)}{/if}
          {/snippet}
        </VirtualTable>
      </section>
    {/if}
  {/if}
</aside>

<style>
  .inspector {
    display: flex;
    flex-direction: column;
    gap: var(--space-4);
    height: 100%;
    padding: var(--space-3) var(--space-4) var(--space-5);
    border-left: 1px solid var(--border);
    background: var(--surface-raised);
    overflow-y: auto;
  }

  .bar {
    display: flex;
    justify-content: space-between;
  }

  .loading {
    display: flex;
    flex-direction: column;
    gap: var(--space-3);
  }

  .title {
    display: flex;
    align-items: center;
    gap: var(--space-2);
    flex-wrap: wrap;
  }

  h2 {
    font-size: var(--text-xl);
    font-weight: 600;
    overflow-wrap: anywhere;
  }

  .tags {
    display: flex;
    gap: var(--space-1);
  }

  .address {
    margin-top: calc(-1 * var(--space-3));
    color: var(--text-faint);
    font-size: var(--text-sm);
  }

  .symbol {
    margin-top: calc(-1 * var(--space-2));
    font-size: var(--text-sm);
    overflow-wrap: anywhere;
  }

  .facts {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: var(--space-2) var(--space-4);
    margin: 0;
  }

  .facts dt {
    color: var(--text-muted);
    font-size: var(--text-xs);
  }

  .facts dd {
    margin: 0;
    font-variant-numeric: tabular-nums;
  }

  section {
    display: flex;
    flex-direction: column;
    gap: var(--space-2);
  }

  h3 {
    font-size: var(--text-md);
    font-weight: 600;
  }

  .section-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--space-2);
  }

  .hint {
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

  .chain {
    display: flex;
    flex-direction: column;
    gap: 2px;
    margin: 0;
    padding: 0 0 0 1.2em;
    font-size: var(--text-sm);
  }

  .chain li {
    padding: 2px 0;
  }

  .chain li > :global(*) {
    vertical-align: middle;
  }

  .paths {
    display: flex;
    flex-direction: column;
    gap: var(--space-2);
    margin: 0;
    padding: 0;
    list-style: none;
    font-size: var(--text-sm);
  }

  .paths li {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 2px var(--space-1);
    padding: var(--space-2);
    border-radius: var(--radius-sm);
    background: var(--surface-sunken);
  }

  .arrow {
    color: var(--text-faint);
  }

  .link {
    display: inline-flex;
    align-items: center;
    gap: var(--space-1);
    max-width: 100%;
    padding: 1px 4px;
    border: 0;
    border-radius: var(--radius-sm);
    background: none;
    color: var(--accent);
    font: inherit;
    cursor: pointer;
  }

  .link:hover {
    background: var(--accent-soft);
  }

  .edge {
    color: var(--text-muted);
  }

  .muted {
    color: var(--text-muted);
  }

  .num {
    margin-left: var(--space-2);
    font-variant-numeric: tabular-nums;
  }
</style>
