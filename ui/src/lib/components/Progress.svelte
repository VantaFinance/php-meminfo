<script lang="ts">
  import { formatBytes, formatCount } from '../format'
  import type { LoadProgress } from '../state/app.svelte'

  interface Props {
    title: string
    progress: LoadProgress
    onCancel: () => void
  }

  let { title, progress, onCancel }: Props = $props()

  const phases = [
    { key: 'reading', label: 'Reading the dump' },
    { key: 'resolving', label: 'Linking items to their children' },
    { key: 'analyzing', label: 'Working out what retains memory' },
  ] as const

  const current = $derived(phases.findIndex((phase) => phase.key === progress.phase))
  const fraction = $derived(progress.totalBytes > 0 ? Math.min(1, progress.bytes / progress.totalBytes) : 0)
</script>

<section class="progress" aria-live="polite" aria-busy="true">
  <h1>{title}</h1>

  <ol class="phases">
    {#each phases as phase, i (phase.key)}
      <li class:done={i < current} class:current={i === current}>
        <span class="marker" aria-hidden="true">{i < current ? '✓' : i + 1}</span>
        {phase.label}
      </li>
    {/each}
  </ol>

  <div
    class="bar"
    role="progressbar"
    aria-valuemin={0}
    aria-valuemax={100}
    aria-valuenow={Math.round(fraction * 100)}
    aria-label="Read so far"
  >
    <div class="fill" style:width="{progress.phase === 'reading' ? fraction * 100 : 100}%"></div>
  </div>
  <p class="facts">
    {formatBytes(progress.bytes)} of {formatBytes(progress.totalBytes)} · {formatCount(progress.items)} items
  </p>

  <button type="button" class="button" onclick={onCancel}>Cancel</button>
</section>

<style>
  .progress {
    display: flex;
    flex-direction: column;
    gap: var(--space-4);
    width: min(480px, 100% - 2 * var(--space-5));
    margin: 18vh auto 0;
  }

  h1 {
    font-size: var(--text-xl);
    font-weight: 600;
    overflow-wrap: anywhere;
  }

  .phases {
    display: flex;
    flex-direction: column;
    gap: var(--space-2);
    margin: 0;
    padding: 0;
    list-style: none;
    color: var(--text-faint);
    font-size: var(--text-sm);
  }

  .phases li {
    display: flex;
    align-items: center;
    gap: var(--space-2);
  }

  .phases li.done {
    color: var(--text-muted);
  }

  .phases li.current {
    color: var(--text);
    font-weight: 600;
  }

  .marker {
    display: inline-grid;
    place-items: center;
    width: 20px;
    height: 20px;
    border: 1px solid currentColor;
    border-radius: 50%;
    font-size: var(--text-xs);
  }

  li.current .marker {
    border-color: var(--accent);
    color: var(--accent);
  }

  .bar {
    height: 6px;
    border-radius: 3px;
    background: var(--surface-sunken);
    overflow: hidden;
  }

  .fill {
    height: 100%;
    background: var(--accent);
    transition: width var(--motion);
  }

  .facts {
    color: var(--text-muted);
    font-size: var(--text-sm);
    font-variant-numeric: tabular-nums;
  }

  button {
    align-self: flex-start;
  }
</style>
