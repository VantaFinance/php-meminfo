<script lang="ts">
  import { formatBytes, formatCount } from '../format'
  import { createLogger } from '../log'
  import { app } from '../state/app.svelte'
  import type { RetainersResult } from '../worker/protocol'
  import type { MergedPathNode } from '../engine/analyses/retainers'
  import ItemLabel from './ItemLabel.svelte'

  interface Props {
    target: { key: string; isClass: boolean }
  }

  let { target }: Props = $props()

  const log = createLogger('retainers-panel')

  let skip = $state('')
  let applied = $state('')
  let result = $state.raw<RetainersResult | null>(null)
  let failed = $state<string | null>(null)

  $effect(() => {
    void app.generation
    const client = app.client
    const { key, isClass } = target
    const pattern = applied
    result = null
    failed = null
    if (client === null) return
    let cancelled = false
    client
      .request({ kind: 'retainers', key, isClass, skip: pattern })
      .then((value) => {
        if (cancelled) return
        result = value
        log.debug('retainers shown', { clusters: value.clusters.length })
      })
      .catch((error: unknown) => {
        if (cancelled) return
        failed = String(error)
        log.warn('retainers failed', { error: failed })
      })
    return () => {
      cancelled = true
    }
  })
</script>

{#snippet tree(node: MergedPathNode, depth: number)}
  <li>
    <span class="count num">{formatCount(node.count)}</span>
    <span class="mono step">{node.step}</span>
    {#if node.children.length > 0 && depth < 12}
      <ul>
        {#each node.children as child, at (at)}
          {@render tree(child, depth + 1)}
        {/each}
      </ul>
    {/if}
  </li>
{/snippet}

<section class="panel" aria-label="How the instances are held">
  <header>
    <h3>How {target.key} {target.isClass ? 'instances are' : 'items are'} held</h3>
    <button type="button" class="button quiet" aria-label="Close" onclick={() => app.showRetainers(null)}>✕</button>
  </header>

  <form
    class="skip"
    onsubmit={(event) => {
      event.preventDefault()
      applied = skip
    }}
  >
    <label>
      Skip holders matching
      <input type="text" class="mono" placeholder="e.g. ^Symfony\\|^Doctrine\\" bind:value={skip} />
    </label>
    <button type="submit" class="button">Apply</button>
  </form>

  {#if failed}
    <div class="notice danger" role="alert">{failed}</div>
  {:else if result === null}
    <div class="skeleton" style:height="160px"></div>
  {:else}
    <p class="hint">{formatCount(result.instances)} items, grouped by the shape of their shortest path from a root.</p>

    <h4>Same path, same place in the code</h4>
    <ol class="clusters">
      {#each result.clusters as cluster, at (at)}
        <li>
          <div class="cluster-head">
            <span class="num">{formatCount(cluster.count)} ×</span>
            <span class="num muted">{formatBytes(cluster.retained)}</span>
            <button type="button" class="link" onclick={() => app.select(cluster.item.index)}><ItemLabel row={cluster.item} /></button>
          </div>
          <ol class="steps mono">
            {#each cluster.steps as step, k (k)}<li>{step}</li>{/each}
          </ol>
        </li>
      {/each}
    </ol>

    {#if result.dominators.length > 0}
      <h4>Kept alive by</h4>
      <ul class="groups">
        {#each result.dominators as group (group.key)}
          <li>
            <span class="num">{formatCount(group.members)}</span>
            {#if group.item}
              <button type="button" class="link" onclick={() => group.item && app.select(group.item.index)}>{group.key}</button>
            {:else}
              <span>{group.key}</span>
            {/if}
            <span class="num muted">{formatBytes(group.retained)}</span>
          </li>
        {/each}
      </ul>
    {/if}

    <h4>All paths, merged from the roots</h4>
    <ul class="tree">{@render tree(result.merged, 0)}</ul>
  {/if}
</section>

<style>
  .panel {
    display: flex;
    flex-direction: column;
    gap: var(--space-3);
    padding: var(--space-4);
    border: 1px solid var(--border);
    border-radius: var(--radius-md);
    background: var(--surface-raised);
  }

  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }

  h3 {
    font-size: var(--text-md);
    font-weight: 600;
  }

  h4 {
    font-size: var(--text-sm);
    font-weight: 600;
  }

  .skip {
    display: flex;
    align-items: end;
    gap: var(--space-2);
    font-size: var(--text-sm);
  }

  .skip label {
    display: flex;
    flex-direction: column;
    gap: 2px;
    flex: 1;
  }

  .clusters,
  .groups {
    display: flex;
    flex-direction: column;
    gap: var(--space-2);
    list-style: none;
    padding: 0;
  }

  .cluster-head {
    display: flex;
    align-items: center;
    gap: var(--space-2);
  }

  .steps {
    margin: 2px 0 0 var(--space-4);
    color: var(--text-muted);
    font-size: var(--text-xs);
  }

  .groups li {
    display: flex;
    gap: var(--space-2);
  }

  .tree,
  .tree :global(ul) {
    list-style: none;
    padding-left: var(--space-3);
    font-size: var(--text-xs);
  }

  .count {
    display: inline-block;
    min-width: 56px;
    color: var(--text-muted);
  }

  .step {
    white-space: nowrap;
  }

  .link {
    background: none;
    border: 0;
    padding: 0;
    color: var(--accent-text);
    cursor: pointer;
    text-align: left;
  }
</style>
