<script lang="ts">
  import type { GrowthRow } from '../../engine/analyses/diff'
  import { formatBytes, formatCount, formatDelta } from '../../format'
  import { app } from '../../state/app.svelte'
  import Progress from '../Progress.svelte'
  import VirtualTable, { type Column } from '../VirtualTable.svelte'

  let input: HTMLInputElement | undefined = $state()
  let rows = $state.raw<GrowthRow[] | null>(null)
  let failed = $state<string | null>(null)
  let changedOnly = $state(true)

  $effect(() => {
    void app.generation
    const baseline = app.baseline
    const client = app.client
    rows = null
    failed = null
    if (client === null || baseline === null) return
    let cancelled = false
    client
      .request({ kind: 'diff', baseline })
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

  const shown = $derived(rows === null ? null : changedOnly ? rows.filter((row) => row.countDelta !== 0 || row.sizeDelta !== 0) : rows)

  function pick(event: Event): void {
    const target = event.currentTarget as HTMLInputElement
    const file = target.files?.[0]
    target.value = ''
    if (file !== undefined) void app.openBaseline(file)
  }

  const columns: Column<GrowthRow>[] = [
    { key: 'key', label: 'Type or class', width: 'minmax(180px, 1fr)', value: (row) => row.key },
    { key: 'before', label: 'Before', width: '100px', align: 'right', value: (row) => formatCount(row.before.count), compare: (a, b) => a.before.count - b.before.count },
    { key: 'after', label: 'Now', width: '100px', align: 'right', value: (row) => formatCount(row.after.count), compare: (a, b) => a.after.count - b.after.count },
    { key: 'countDelta', label: 'Δ count', width: '100px', align: 'right', value: (row) => formatDelta(row.countDelta), compare: (a, b) => a.countDelta - b.countDelta },
    { key: 'sizeDelta', label: 'Δ self size', width: '110px', align: 'right', value: (row) => formatDelta(row.sizeDelta, formatBytes), compare: (a, b) => a.sizeDelta - b.sizeDelta },
  ]
</script>

<section class="view">
  <header class="head">
    <div>
      <h2>Growth</h2>
      <p class="sub">
        Compare with an earlier dump of the same process: what keeps growing between the two is what leaks.
      </p>
    </div>
    {#if app.baseline}
      <div class="baseline">
        <span>Baseline <strong>{app.baseline.name}</strong> · {formatCount(app.baseline.items)} items</span>
        <button type="button" class="button quiet" onclick={() => input?.click()}>Change</button>
        <button type="button" class="button quiet" onclick={() => app.clearBaseline()}>Remove</button>
      </div>
    {/if}
    <input bind:this={input} class="visually-hidden" type="file" accept=".json,application/json" onchange={pick} />
  </header>

  {#if app.baselineProgress}
    <Progress title="Reading the baseline" progress={app.baselineProgress} onCancel={() => app.clearBaseline()} />
  {:else if app.baselineError}
    <div class="notice danger" role="alert">The baseline could not be read: {app.baselineError}</div>
  {:else if app.baseline === null}
    <div class="empty">
      <p>
        Load the dump taken <em>earlier</em> as a baseline. Only its totals per class are kept, so comparing does not need
        memory for a second dump.
      </p>
      <button type="button" class="button primary" onclick={() => input?.click()}>Choose baseline dump…</button>
    </div>
  {:else if failed}
    <div class="notice danger" role="alert">{failed}</div>
  {:else if shown === null}
    <div class="skeleton" style:height="240px"></div>
  {:else}
    <label class="toggle">
      <input type="checkbox" bind:checked={changedOnly} />
      Only types that changed
    </label>
    <VirtualTable
      label="Growth since the baseline"
      {columns}
      rows={shown}
      onActivate={(row) => !row.isGone && app.searchFor(row.key, row.isClass)}
      height="calc(100vh - 230px)"
    >
      {#snippet empty()}Nothing changed between the two dumps.{/snippet}
      {#snippet cell(row, column)}
        {#if column.key === 'key'}
          <span class:cls={row.isClass}>{row.key}</span>
          {#if row.isNew}<span class="badge new">new</span>{/if}
          {#if row.isGone}<span class="badge">gone</span>{/if}
        {:else if column.key === 'countDelta' || column.key === 'sizeDelta'}
          {@const delta = column.key === 'countDelta' ? row.countDelta : row.sizeDelta}
          <span class:grew={delta > 0} class:shrank={delta < 0}>{column.value(row)}</span>
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

  .head {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    gap: var(--space-4);
    flex-wrap: wrap;
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

  .baseline {
    display: flex;
    align-items: center;
    gap: var(--space-2);
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

  .empty {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: var(--space-3);
    max-width: 60ch;
    color: var(--text-muted);
  }

  .toggle {
    display: inline-flex;
    align-items: center;
    gap: var(--space-2);
    font-size: var(--text-sm);
    color: var(--text-muted);
  }

  .cls {
    font-weight: 500;
  }

  .grew {
    color: var(--danger);
    font-weight: 500;
  }

  .shrank {
    color: var(--success);
  }
</style>
