<script lang="ts">
  import type { ItemRow } from '../worker/protocol'

  interface Props {
    row: ItemRow
    /** Show the address too. */
    address?: boolean
  }

  let { row, address = false }: Props = $props()
</script>

<span class="label">
  <span class="kind">{row.className ?? row.type}</span>
  {#if row.symbol}<span class="symbol mono">{row.symbol}</span>{/if}
  {#if row.isRoot}<span class="badge root">root</span>{/if}
  {#if address}<span class="address mono">{row.address}</span>{/if}
</span>

<style>
  .label {
    display: inline-flex;
    align-items: center;
    gap: var(--space-2);
    min-width: 0;
    overflow: hidden;
  }

  .kind {
    font-weight: 500;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .symbol {
    color: var(--text-muted);
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .address {
    color: var(--text-faint);
  }
</style>
