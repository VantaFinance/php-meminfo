<script lang="ts">
  import { formatBytes, formatCount } from '../../format'
  import { app } from '../../state/app.svelte'
  import type { TopChildrenRowView } from '../../worker/protocol'
  import AddressLink from '../AddressLink.svelte'
  import ItemLabel from '../ItemLabel.svelte'
  import VirtualTable, { type Column } from '../VirtualTable.svelte'

  let rows = $state.raw<TopChildrenRowView[] | null>(null)
  let failed = $state<string | null>(null)

  $effect(() => {
    void app.generation
    const client = app.client
    if (client === null) return
    let cancelled = false
    client
      .request({ kind: 'topChildren', limit: 500 })
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

  const columns: Column<TopChildrenRowView>[] = [
    { key: 'item', label: 'Item', width: 'minmax(220px, 1fr)', value: (row) => `${row.className ?? row.type} ${row.symbol ?? ''}` },
    { key: 'address', label: 'Address', width: '140px', value: (row) => row.address },
    { key: 'unique', label: 'Children', width: '100px', align: 'right', value: (row) => formatCount(row.uniqueKeys), compare: (a, b) => a.uniqueKeys - b.uniqueKeys },
    {
      key: 'edges',
      label: 'Edges',
      width: '90px',
      align: 'right',
      value: (row) => formatCount(row.children),
      title: (row) => (row.children !== row.uniqueKeys ? 'Includes duplicate names (same-named private properties, keys cut at a NUL byte)' : ''),
      compare: (a, b) => a.children - b.children,
    },
    { key: 'retained', label: 'Retained', width: '100px', align: 'right', value: (row) => (row.retained === undefined ? '—' : formatBytes(row.retained)), compare: (a, b) => (a.retained ?? 0) - (b.retained ?? 0) },
  ]
</script>

<section class="view">
  <header>
    <h2>Top children</h2>
    <p class="sub">Arrays and objects with the most children — a growing collection is the usual shape of a leak.</p>
  </header>

  {#if failed}
    <div class="notice danger" role="alert">{failed}</div>
  {:else if rows === null}
    <div class="skeleton" style:height="240px"></div>
  {:else}
    <VirtualTable
      label="Items with the most children"
      {columns}
      {rows}
      rowKey={(row) => row.index}
      selectedKey={app.selected}
      onActivate={(row) => app.select(row.index)}
      height="calc(100vh - 190px)"
    >
      {#snippet cell(row, column)}
        {#if column.key === 'item'}
          <ItemLabel {row} />
        {:else if column.key === 'address'}
          <AddressLink index={row.index} address={row.address} />
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

</style>
