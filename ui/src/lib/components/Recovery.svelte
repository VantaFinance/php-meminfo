<script lang="ts">
  import { formatBytes } from '../format'
  import type { CrashReport } from '../state/app.svelte'

  interface Props {
    report: CrashReport
    onDismiss: () => void
  }

  let { report, onDismiss }: Props = $props()
</script>

<section class="recovery" role="alert">
  <h1>The browser ran out of memory</h1>
  <p>
    Analyzing <strong>{report.fileName}</strong> ({formatBytes(report.fileSize)}) stopped while
    {report.phase === 'browsing' ? 'browsing it' : `in the “${report.phase}” phase`}. The tab had to drop it.
  </p>
  <ul>
    <li>Close other tabs and try again — the dump needs room on top of what they use.</li>
    <li>Chrome handles the largest dumps (about 2.5 GB); other browsers may stop earlier.</li>
    <li>Take a smaller dump: dump earlier, or reproduce the leak with less traffic.</li>
  </ul>
  <button type="button" class="button primary" onclick={onDismiss}>Open another dump</button>
</section>

<style>
  .recovery {
    display: flex;
    flex-direction: column;
    gap: var(--space-3);
    width: min(560px, 100% - 2 * var(--space-5));
    margin: 14vh auto 0;
  }

  h1 {
    font-size: var(--text-xl);
    font-weight: 600;
  }

  p,
  ul {
    margin: 0;
    color: var(--text-muted);
  }

  ul {
    padding-left: 1.2em;
  }

  li + li {
    margin-top: var(--space-1);
  }

  button {
    align-self: flex-start;
    margin-top: var(--space-2);
  }
</style>
