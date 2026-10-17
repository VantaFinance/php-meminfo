<script lang="ts">
  import type { SummaryRow } from '../../engine/analyses/basic'
  import { formatBytes, formatCount, formatPercent } from '../../format'
  import { app } from '../../state/app.svelte'
  import VirtualTable, { type Column } from '../VirtualTable.svelte'

  let rows = $state.raw<SummaryRow[] | null>(null)
  let failed = $state<string | null>(null)

  $effect(() => {
    void app.generation
    const client = app.client
    if (client === null) return
    let cancelled = false
    client
      .request({ kind: 'summary' })
      .then((value) => {
        if (!cancelled) rows = value
      })
      .catch((error: unknown) => {
        if (!cancelled) failed = String(error)
      })
    return () => {
      cancelled = true
    }
  })

  const totalCount = $derived(rows?.reduce((sum, row) => sum + row.count, 0) ?? 0)
  const totalSize = $derived(rows?.reduce((sum, row) => sum + row.selfSize, 0) ?? 0)

  const columns: Column<SummaryRow>[] = [
    { key: 'key', label: 'Type or class', width: 'minmax(180px, 1fr)', value: (row) => row.key, compare: (a, b) => b.key.localeCompare(a.key) },
    { key: 'count', label: 'Count', width: '110px', align: 'right', value: (row) => formatCount(row.count), compare: (a, b) => a.count - b.count },
    { key: 'countShare', label: '% items', width: '80px', align: 'right', value: (row) => formatPercent(row.count, totalCount) },
    { key: 'size', label: 'Self size', width: '110px', align: 'right', value: (row) => formatBytes(row.selfSize), compare: (a, b) => a.selfSize - b.selfSize },
    { key: 'sizeShare', label: '% size', width: '80px', align: 'right', value: (row) => formatPercent(row.selfSize, totalSize) },
  ]
</script>

<section class="view">
  <header>
    <h2>Summary</h2>
    <p class="sub">Every item in the dump, grouped by type — objects by class. Select a row to list its instances.</p>
  </header>

  {#if failed}
    <div class="notice danger" role="alert">{failed}</div>
  {:else if rows === null}
    <div class="skeleton" style:height="240px"></div>
  {:else}
    <VirtualTable
      label="Summary by type and class"
      {columns}
      {rows}
      onActivate={(row) => app.searchFor(row.key, row.isClass)}
      height="calc(100vh - 190px)"
    >
      {#snippet cell(row, column)}
        {#if column.key === 'key'}
          <span class:cls={row.isClass}>{row.key}</span>
        {:else}
          {column.value(row)}
        {/if}
      {/snippet}
    </VirtualTable>
  {/if}
</section>

<style>
  .view {
    display: flex;
    flex-direction: column;
    gap: var(--space-4);
    padding: var(--space-5);
  }

  h2 {
    font-size: var(--text-xl);
    font-weight: 600;
  }

  .sub {
    margin-top: 2px;
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

  .cls {
    font-weight: 500;
  }
</style>
