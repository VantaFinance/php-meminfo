<script lang="ts">
  import { formatBytes } from '../format'
  import { app } from '../state/app.svelte'

  let input: HTMLInputElement | undefined = $state()
  let dragging = $state(false)

  function pick(event: Event): void {
    const target = event.currentTarget as HTMLInputElement
    const file = target.files?.[0]
    // Drop the input's reference at once: the File must not outlive the hand-off.
    target.value = ''
    if (file !== undefined) app.open(file)
  }

  function drop(event: DragEvent): void {
    event.preventDefault()
    dragging = false
    const file = event.dataTransfer?.files[0]
    if (file !== undefined) app.open(file)
  }
</script>

<section class="panel">
  <div class="intro">
    <h1>PHP Meminfo</h1>
    <p class="lead">Find what keeps memory alive in a PHP process, from a meminfo dump.</p>
  </div>

  {#if app.error}
    <div class="notice danger" role="alert">
      <strong>{app.error.fileName} could not be opened.</strong>
      {app.error.message}
    </div>
  {/if}

  {#if app.pendingLarge}
    <div class="notice warning" role="alert">
      <p>
        <strong>{app.pendingLarge.name}</strong> is {formatBytes(app.pendingLarge.size)} — more than this browser is
        expected to hold (about 2.5 GB of dump in Chrome). Loading it may run out of memory.
      </p>
      <div class="actions">
        <button type="button" class="button" onclick={() => app.pendingLarge && app.open(app.pendingLarge, true)}>Load anyway</button>
        <button type="button" class="button quiet" onclick={() => (app.pendingLarge = null)}>Cancel</button>
      </div>
    </div>
  {/if}

  <div
    class="drop"
    class:dragging
    role="region"
    aria-label="Drop a dump file here"
    ondragover={(event) => {
      event.preventDefault()
      dragging = true
    }}
    ondragleave={() => (dragging = false)}
    ondrop={drop}
  >
    <p class="drop-title">Drop a dump file here</p>
    <p class="drop-hint">or</p>
    <button type="button" class="button primary" onclick={() => input?.click()}>Open dump…</button>
    <input bind:this={input} class="visually-hidden" type="file" accept=".json,application/json" onchange={pick} />
    <p class="privacy">The dump is read and analyzed in this browser. Nothing is uploaded.</p>
  </div>

  <div class="howto">
    <h2>Getting a dump</h2>
    <p>From a running process, with the meminfo extension loaded:</p>
    <pre><code>dump-meminfo &lt;pid&gt; --dir=/tmp</code></pre>
    <p>From the application itself:</p>
    <pre><code>meminfo_dump(fopen('/tmp/dump.json', 'w'));</code></pre>
    <p class="tip">
      To see what grows, take one dump, let the process run, take another, and compare them in the
      <em>Growth</em> view.
    </p>
  </div>
</section>

<style>
  .panel {
    display: flex;
    flex-direction: column;
    gap: var(--space-5);
    width: min(640px, 100% - 2 * var(--space-5));
    margin: 10vh auto var(--space-6);
  }

  h1 {
    font-size: var(--text-2xl);
    font-weight: 650;
    letter-spacing: -0.01em;
  }

  .lead {
    margin-top: var(--space-1);
    color: var(--text-muted);
    font-size: var(--text-lg);
  }

  .drop {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--space-2);
    padding: var(--space-6) var(--space-5);
    border: 1.5px dashed var(--border-strong);
    border-radius: var(--radius-md);
    background: var(--surface-sunken);
    transition: border-color var(--motion), background var(--motion);
  }

  .drop.dragging {
    border-color: var(--accent);
    background: var(--accent-soft);
  }

  .drop-title {
    font-size: var(--text-lg);
    font-weight: 600;
  }

  .drop-hint,
  .privacy {
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

  .privacy {
    margin-top: var(--space-2);
  }

  .howto {
    display: flex;
    flex-direction: column;
    gap: var(--space-2);
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

  h2 {
    color: var(--text);
    font-size: var(--text-md);
    font-weight: 600;
  }

  pre {
    margin: 0;
    padding: var(--space-2) var(--space-3);
    border-radius: var(--radius-sm);
    background: var(--surface-sunken);
    color: var(--text);
    overflow-x: auto;
  }

  .tip {
    margin-top: var(--space-2);
  }

  .notice p {
    margin-bottom: var(--space-2);
  }

  .actions {
    display: flex;
    gap: var(--space-2);
  }
</style>
