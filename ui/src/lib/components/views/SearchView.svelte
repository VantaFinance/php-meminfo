<script lang="ts">
  import type { QueryAttribute, QueryFilter } from '../../engine/analyses/basic'
  import { formatBytes, formatCount } from '../../format'
  import { app } from '../../state/app.svelte'
  import type { ItemRow } from '../../worker/protocol'
  import { WorkerFailure } from '../../worker/client'
  import AddressLink from '../AddressLink.svelte'
  import ItemLabel from '../ItemLabel.svelte'
  import VirtualTable, { type Column } from '../VirtualTable.svelte'

  const ATTRIBUTES: Array<{ value: QueryAttribute; label: string }> = [
    { value: 'class', label: 'class' },
    { value: 'type', label: 'type' },
    { value: 'symbol_name', label: 'symbol' },
    { value: 'frame', label: 'frame' },
    { value: 'is_root', label: 'is root' },
    { value: 'size', label: 'size' },
    { value: 'object_handle', label: 'object handle' },
    { value: 'id', label: 'address' },
  ]

  let filters = $state<QueryFilter[]>([{ attribute: 'class', operator: '=', value: '' }])
  let submitted = $state.raw<{ filters: QueryFilter[]; total: number; key: number } | null>(null)
  let error = $state<string | null>(null)
  let running = $state(false)
  let address = $state('')
  let addressError = $state<string | null>(null)
  let key = 0

  // Another view linked here with filters: start from them and run at once.
  $effect(() => {
    const preset = app.searchFilters
    if (preset.length === 0) return
    filters = preset.map((filter) => ({ ...filter }))
    app.searchFilters = []
    void run()
  })

  $effect(() => {
    void app.generation
    submitted = null
  })

  async function run(): Promise<void> {
    const client = app.client
    if (client === null) return
    // A snapshot: the form state is a deep proxy, which postMessage cannot clone.
    const active = $state.snapshot(filters).filter((filter) => filter.value.trim() !== '' || filter.attribute === 'is_root')
    error = null
    running = true
    try {
      const page = await client.request({ kind: 'query', filters: active, offset: 0, limit: 1 })
      submitted = { filters: active, total: page.total, key: ++key }
    } catch (failure) {
      error = failure instanceof WorkerFailure ? failure.message : String(failure)
      submitted = null
    } finally {
      running = false
    }
  }

  async function load(offset: number, limit: number): Promise<ItemRow[]> {
    const client = app.client
    if (client === null || submitted === null) return []
    const page = await client.request({ kind: 'query', filters: submitted.filters, offset, limit })
    return page.rows
  }

  async function jump(): Promise<void> {
    const client = app.client
    if (client === null || address.trim() === '') return
    addressError = null
    const index = await client.request({ kind: 'find', address })
    if (index < 0) addressError = 'No item has this address in this dump.'
    else app.select(index)
  }

  const columns: Column<ItemRow>[] = [
    { key: 'item', label: 'Item', width: 'minmax(220px, 1fr)', value: (row) => `${row.className ?? row.type} ${row.symbol ?? ''}` },
    { key: 'address', label: 'Address', width: '140px', value: (row) => row.address },
    { key: 'frame', label: 'Frame', width: 'minmax(100px, 0.5fr)', value: (row) => row.frame ?? '' },
    { key: 'size', label: 'Self size', width: '90px', align: 'right', value: (row) => formatBytes(row.size) },
    { key: 'retained', label: 'Retained', width: '96px', align: 'right', value: (row) => (row.retained === undefined ? '—' : formatBytes(row.retained)) },
    { key: 'children', label: 'Children', width: '84px', align: 'right', value: (row) => formatCount(row.children) },
  ]
</script>

<section class="view">
  <header>
    <h2>Search</h2>
    <p class="sub">Find items by their fields. Every filter must match; <code>~</code> takes a regular expression.</p>
  </header>

  <form
    class="filters"
    onsubmit={(event) => {
      event.preventDefault()
      void run()
    }}
  >
    {#each filters as filter, i (i)}
      <div class="filter">
        <select bind:value={filter.attribute} aria-label="Field">
          {#each ATTRIBUTES as attribute (attribute.value)}
            <option value={attribute.value}>{attribute.label}</option>
          {/each}
        </select>
        {#if filter.attribute === 'is_root'}
          <select bind:value={filter.value} aria-label="Value">
            <option value="1">yes</option>
            <option value="0">no</option>
          </select>
        {:else}
          <select bind:value={filter.operator} aria-label="Operator">
            <option value="=">is</option>
            <option value="~">matches</option>
          </select>
          <input type="text" bind:value={filter.value} aria-label="Value" placeholder={filter.operator === '~' ? 'regular expression' : 'value'} />
        {/if}
        <button type="button" class="button quiet" aria-label="Remove filter" disabled={filters.length === 1} onclick={() => filters.splice(i, 1)}>✕</button>
      </div>
    {/each}
    <div class="actions">
      <button type="button" class="button quiet" onclick={() => filters.push({ attribute: 'type', operator: '=', value: '' })}>Add filter</button>
      <button type="submit" class="button primary" disabled={running}>{running ? 'Searching…' : 'Search'}</button>
    </div>
  </form>

  <form
    class="jump"
    onsubmit={(event) => {
      event.preventDefault()
      void jump()
    }}
  >
    <input type="text" bind:value={address} placeholder="0x7f…" aria-label="Address" aria-invalid={addressError !== null} />
    <button type="submit" class="button">Go to address</button>
    {#if addressError}<span class="error">{addressError}</span>{/if}
  </form>

  {#if error}
    <div class="notice danger" role="alert">{error}</div>
  {:else if submitted}
    <p class="count">{formatCount(submitted.total)} {submitted.total === 1 ? 'item matches' : 'items match'}</p>
    <VirtualTable
      label="Search results"
      {columns}
      total={submitted.total}
      {load}
      resetKey={submitted.key}
      rowKey={(row) => row.index}
      selectedKey={app.selected}
      onActivate={(row) => app.select(row.index)}
      height="calc(100vh - 360px)"
    >
      {#snippet empty()}No item matches these filters.{/snippet}
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

  .filters {
    display: flex;
    flex-direction: column;
    gap: var(--space-2);
  }

  .filter,
  .jump,
  .actions {
    display: flex;
    align-items: center;
    gap: var(--space-2);
    flex-wrap: wrap;
  }

  .filter input {
    flex: 1;
    min-width: 160px;
    max-width: 420px;
  }

  .jump input {
    width: 220px;
    font-family: var(--font-mono);
  }

  .error {
    color: var(--danger);
    font-size: var(--text-sm);
  }

  .count {
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

</style>
