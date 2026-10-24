<script lang="ts">
  import { formatBytes, formatCount } from '../../format'
  import { createLogger } from '../../log'
  import { app } from '../../state/app.svelte'
  import type { FindingRow } from '../../worker/protocol'
  import ItemLabel from '../ItemLabel.svelte'

  const log = createLogger('findings-view')

  let findings = $state.raw<FindingRow[] | null>(null)
  let failed = $state<string | null>(null)

  $effect(() => {
    void app.generation
    const client = app.client
    if (client === null) return
    let cancelled = false
    client
      .request({ kind: 'findings' })
      .then((value) => {
        if (cancelled) return
        findings = value
        log.debug('findings shown', { count: value.length })
      })
      .catch((error: unknown) => {
        if (cancelled) return
        failed = String(error)
        log.warn('findings failed', { error: failed })
      })
    return () => {
      cancelled = true
    }
  })
</script>

<section class="view">
  <header>
    <h2>Findings</h2>
    <p class="sub">Patterns that usually mean a leak or wasted memory, each with what to look at next.</p>
  </header>

  {#if failed}
    <div class="notice danger" role="alert">{failed}</div>
  {:else if findings === null}
    <div class="skeleton" style:height="240px"></div>
  {:else if findings.length === 0}
    <p class="notice">Nothing stands out in this dump. Compare it with a later one (Growth) to see what keeps growing.</p>
  {:else}
    <ol class="findings">
      {#each findings as finding, at (at)}
        <li class="finding {finding.severity}">
          <div class="head">
            <span class="severity">{finding.severity}</span>
            <h3>{finding.title}</h3>
            <span class="bytes num">{formatBytes(finding.bytes)}</span>
          </div>
          <p>{finding.hypothesis}</p>
          <p class="next">{finding.next}</p>
          <div class="actions">
            {#if finding.item}
              <button type="button" class="button quiet" onclick={() => finding.item && app.select(finding.item.index)}>
                <ItemLabel row={finding.item} />
              </button>
              <button type="button" class="button quiet" onclick={() => finding.item && app.openGraph(finding.item.index)}>Show graph</button>
            {/if}
            {#if finding.className}
              <button type="button" class="button quiet" onclick={() => finding.className && app.searchFor(finding.className, true)}>
                List {formatCount(finding.count)} instances
              </button>
            {/if}
          </div>
        </li>
      {/each}
    </ol>
  {/if}
</section>

<style>
  .view {
    display: flex;
    flex-direction: column;
    gap: var(--space-4);
    padding: var(--space-5);
    overflow: auto;
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

  .findings {
    display: flex;
    flex-direction: column;
    gap: var(--space-3);
    list-style: none;
    padding: 0;
  }

  .finding {
    display: flex;
    flex-direction: column;
    gap: var(--space-2);
    padding: var(--space-3) var(--space-4);
    border: 1px solid var(--border);
    border-left: 3px solid var(--border-strong);
    border-radius: var(--radius-md);
    background: var(--surface-raised);
  }

  .finding.high {
    border-left-color: var(--danger);
  }

  .finding.medium {
    border-left-color: var(--warning);
  }

  .head {
    display: flex;
    align-items: baseline;
    gap: var(--space-3);
  }

  h3 {
    flex: 1;
    min-width: 0;
    font-size: var(--text-md);
    font-weight: 600;
  }

  .severity {
    font-size: var(--text-xs);
    text-transform: uppercase;
    color: var(--text-muted);
  }

  .bytes {
    color: var(--text-muted);
  }

  .next {
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: var(--space-2);
  }
</style>
