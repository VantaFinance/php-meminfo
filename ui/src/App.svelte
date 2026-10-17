<script lang="ts">
  import Inspector from './lib/components/Inspector.svelte'
  import LoadPanel from './lib/components/LoadPanel.svelte'
  import Progress from './lib/components/Progress.svelte'
  import Recovery from './lib/components/Recovery.svelte'
  import GraphView from './lib/components/views/GraphView.svelte'
  import GrowthView from './lib/components/views/GrowthView.svelte'
  import SearchView from './lib/components/views/SearchView.svelte'
  import SummaryView from './lib/components/views/SummaryView.svelte'
  import SuspectsView from './lib/components/views/SuspectsView.svelte'
  import TopChildrenView from './lib/components/views/TopChildrenView.svelte'
  import { formatBytes, formatCount, formatDuration } from './lib/format'
  import { app, type View } from './lib/state/app.svelte'

  const NAV: Array<{ view: View; label: string; hint: string }> = [
    { view: 'suspects', label: 'Leak suspects', hint: 'What retains the most' },
    { view: 'growth', label: 'Growth', hint: 'Compare with an earlier dump' },
    { view: 'summary', label: 'Summary', hint: 'Counts by type and class' },
    { view: 'top', label: 'Top children', hint: 'The largest collections' },
    { view: 'search', label: 'Search', hint: 'Find items by field' },
    { view: 'graph', label: 'Graph', hint: 'What keeps an item reachable' },
  ]

  let input: HTMLInputElement | undefined = $state()
  let dragging = $state(false)

  function pick(event: Event): void {
    const target = event.currentTarget as HTMLInputElement
    const file = target.files?.[0]
    target.value = ''
    if (file !== undefined) app.open(file)
  }

  // A file dropped anywhere replaces the current dump.
  function onDrop(event: DragEvent): void {
    if (!event.dataTransfer?.types.includes('Files')) return
    event.preventDefault()
    dragging = false
    const file = event.dataTransfer.files[0]
    if (file !== undefined && app.status !== 'loading') app.open(file)
  }

  function onDragOver(event: DragEvent): void {
    if (!event.dataTransfer?.types.includes('Files')) return
    event.preventDefault()
    dragging = app.status === 'ready'
  }
</script>

<svelte:window ondragover={onDragOver} ondrop={onDrop} ondragleave={(event) => event.relatedTarget === null && (dragging = false)} />

<input bind:this={input} class="visually-hidden" type="file" accept=".json,application/json" onchange={pick} aria-label="Open dump" />

{#if app.status === 'loading' && app.progress}
  <Progress title={`Opening ${app.progress.totalBytes > 0 ? formatBytes(app.progress.totalBytes) : ''} dump`} progress={app.progress} onCancel={() => app.cancel()} />
{:else if app.status === 'crashed' && app.crash}
  <Recovery report={app.crash} onDismiss={() => app.dismiss()} />
{:else if app.status === 'ready' && app.overview}
  {@const overview = app.overview}
  <div class="shell" class:with-inspector={app.selected !== null}>
    <header class="topbar">
      <div class="brand">PHP Meminfo</div>
      <div class="file">
        <span class="name" title={overview.fileName}>{overview.fileName}</span>
        <span class="facts">
          {formatBytes(overview.fileSize)} · {formatCount(overview.stats.items)} items · {formatCount(overview.roots)} roots
          {#if overview.header.memory_usage}· {formatBytes(overview.header.memory_usage)} in use{/if}
          · read in {formatDuration(overview.parseMs)}
        </span>
      </div>
      <div class="actions">
        <button type="button" class="button" onclick={() => input?.click()}>Open another…</button>
        <button type="button" class="button quiet" onclick={() => app.close()}>Close</button>
      </div>
    </header>

    <nav class="nav" aria-label="Views">
      {#each NAV as item (item.view)}
        <button type="button" class="nav-item" class:current={app.view === item.view} aria-current={app.view === item.view ? 'page' : undefined} onclick={() => (app.view = item.view)}>
          <span class="nav-label">{item.label}</span>
          <span class="nav-hint">{item.hint}</span>
        </button>
      {/each}
      {#if overview.stats.danglingEdges > 0 || overview.stats.duplicateAddresses > 0}
        <p class="nav-warning">
          {#if overview.stats.danglingEdges > 0}{formatCount(overview.stats.danglingEdges)} children point outside the dump (it was probably cut short).{/if}
          {#if overview.stats.duplicateAddresses > 0}{formatCount(overview.stats.duplicateAddresses)} duplicate addresses.{/if}
        </p>
      {/if}
    </nav>

    <main class="main">
      {#if app.view === 'suspects'}
        <SuspectsView />
      {:else if app.view === 'growth'}
        <GrowthView />
      {:else if app.view === 'summary'}
        <SummaryView />
      {:else if app.view === 'top'}
        <TopChildrenView />
      {:else if app.view === 'graph'}
        <GraphView />
      {:else}
        <SearchView />
      {/if}
    </main>

    {#if app.selected !== null}
      <div class="side">
        <Inspector index={app.selected} />
      </div>
    {/if}

    {#if dragging}
      <div class="drop-overlay" aria-hidden="true">Drop to open this dump instead</div>
    {/if}
  </div>
{:else}
  <LoadPanel />
{/if}

<style>
  .shell {
    display: grid;
    grid-template-columns: 216px minmax(0, 1fr);
    grid-template-rows: 52px minmax(0, 1fr);
    grid-template-areas:
      'top top'
      'nav main';
    height: 100%;
  }

  .shell.with-inspector {
    grid-template-columns: 216px minmax(0, 1fr) minmax(360px, 440px);
    grid-template-areas:
      'top top top'
      'nav main side';
  }

  .topbar {
    grid-area: top;
    display: flex;
    align-items: center;
    gap: var(--space-4);
    padding: 0 var(--space-4);
    border-bottom: 1px solid var(--border);
    background: var(--surface-raised);
  }

  .brand {
    font-weight: 650;
    white-space: nowrap;
  }

  .file {
    display: flex;
    flex-direction: column;
    min-width: 0;
    flex: 1;
    line-height: 1.25;
  }

  .name {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-weight: 500;
  }

  .facts {
    color: var(--text-muted);
    font-size: var(--text-xs);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    font-variant-numeric: tabular-nums;
  }

  .actions {
    display: flex;
    gap: var(--space-2);
  }

  .nav {
    grid-area: nav;
    display: flex;
    flex-direction: column;
    gap: 2px;
    padding: var(--space-3) var(--space-2);
    border-right: 1px solid var(--border);
    background: var(--surface-nav);
    overflow-y: auto;
  }

  .nav-item {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 1px;
    padding: var(--space-2) var(--space-3);
    border: 0;
    border-radius: var(--radius-md);
    background: none;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
    transition: background var(--motion);
  }

  .nav-item:hover {
    background: var(--surface-hover);
  }

  .nav-item.current {
    background: var(--accent-soft);
  }

  .nav-item.current .nav-label {
    color: var(--accent);
  }

  .nav-label {
    font-size: var(--text-sm);
    font-weight: 600;
  }

  .nav-hint {
    color: var(--text-muted);
    font-size: var(--text-xs);
  }

  .nav-warning {
    margin-top: auto;
    padding: var(--space-2) var(--space-3);
    color: var(--warning);
    font-size: var(--text-xs);
  }

  .main {
    grid-area: main;
    min-width: 0;
    overflow-y: auto;
  }

  .side {
    grid-area: side;
    min-height: 0;
  }

  .drop-overlay {
    position: fixed;
    inset: var(--space-3);
    display: grid;
    place-items: center;
    border: 2px dashed var(--accent);
    border-radius: var(--radius-md);
    background: color-mix(in srgb, var(--accent-soft) 85%, transparent);
    color: var(--accent);
    font-size: var(--text-lg);
    font-weight: 600;
    pointer-events: none;
  }

  @media (max-width: 1100px) {
    .shell.with-inspector {
      grid-template-columns: 216px minmax(0, 1fr);
      grid-template-rows: 52px minmax(0, 1fr) minmax(0, 45vh);
      grid-template-areas:
        'top top'
        'nav main'
        'nav side';
    }

    .side :global(.inspector) {
      border-left: 0;
      border-top: 1px solid var(--border);
    }
  }

  @media (max-width: 760px) {
    .shell,
    .shell.with-inspector {
      grid-template-columns: minmax(0, 1fr);
      grid-template-rows: auto auto minmax(0, 1fr) auto;
      grid-template-areas: 'top' 'nav' 'main' 'side';
    }

    .topbar {
      flex-wrap: wrap;
      padding: var(--space-2) var(--space-3);
    }

    .nav {
      flex-direction: row;
      overflow-x: auto;
      border-right: 0;
      border-bottom: 1px solid var(--border);
    }

    .nav-hint {
      display: none;
    }
  }
</style>
