<script lang="ts">
  import type { RetainerFilter } from '../engine/analyses/filters'
  import { app } from '../state/app.svelte'

  let open = $state(false)
  let kind = $state<'holders' | 'edge'>('holders')
  let pattern = $state('')
  let holder = $state('')
  let name = $state('')

  function describe(filter: RetainerFilter): string {
    return filter.kind === 'holders' ? `holders /${filter.pattern}/` : `${filter.holder}->${filter.name}`
  }

  function add(event: SubmitEvent): void {
    event.preventDefault()
    const filter: RetainerFilter | null =
      kind === 'holders'
        ? pattern.trim() === '' ? null : { kind: 'holders', pattern: pattern.trim() }
        : holder.trim() === '' || name.trim() === '' ? null : { kind: 'edge', holder: holder.trim(), name: name.trim() }
    if (filter === null) return
    void app.setRetainerFilters([...app.retainerFilters, filter])
    pattern = ''
    name = ''
  }

  function remove(at: number): void {
    void app.setRetainerFilters(app.retainerFilters.filter((_, k) => k !== at))
  }
</script>

<div class="filters" aria-label="Ignored holders">
  <span class="label">Ignored holders</span>
  {#each app.retainerFilters as filter, at (at)}
    <span class="chip mono">
      {describe(filter)}
      <button type="button" aria-label="Stop ignoring {describe(filter)}" onclick={() => remove(at)}>✕</button>
    </span>
  {:else}
    <span class="muted">none — everything that references an item counts as holding it</span>
  {/each}
  {#if app.retainerFilters.length > 0}
    <button type="button" class="button quiet" onclick={() => void app.setRetainerFilters([])}>Clear</button>
  {/if}
  <button type="button" class="button quiet" aria-expanded={open} onclick={() => (open = !open)}>Ignore…</button>
  {#if app.filtersError}<span class="error" role="alert">{app.filtersError}</span>{/if}

  {#if open}
    <form class="add" onsubmit={add}>
      <select bind:value={kind} aria-label="What to ignore">
        <option value="holders">Every reference from classes matching</option>
        <option value="edge">One property or key of a class</option>
      </select>
      {#if kind === 'holders'}
        <input class="mono" placeholder="^Monolog\\|Cache$" bind:value={pattern} aria-label="Class pattern" />
      {:else}
        <input class="mono" placeholder="App\Registry" bind:value={holder} aria-label="Class or type" />
        <input class="mono" placeholder="listeners" bind:value={name} aria-label="Property or key" />
      {/if}
      <button type="submit" class="button">Ignore and recompute</button>
    </form>
  {/if}
</div>

<style>
  .filters {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: var(--space-2);
    padding: var(--space-2) var(--space-5);
    border-bottom: 1px solid var(--border);
    font-size: var(--text-sm);
  }

  .label {
    font-weight: 600;
  }

  .chip {
    display: inline-flex;
    align-items: center;
    gap: var(--space-1);
    padding: 2px var(--space-2);
    border-radius: var(--radius-sm);
    background: var(--warning-soft);
  }

  .chip button {
    border: 0;
    background: none;
    cursor: pointer;
    color: var(--text-muted);
  }

  .add {
    display: flex;
    flex-wrap: wrap;
    gap: var(--space-2);
    width: 100%;
  }

  .error {
    color: var(--danger);
  }
</style>
