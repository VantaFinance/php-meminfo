<script lang="ts">
  import { createLogger } from '../log'
  import { app } from '../state/app.svelte'

  /**
   * An item's address as a link to its graph. Inside a table row it must not
   * also trigger the row (which opens the inspector): the click and the Enter
   * key stop here.
   */
  interface Props {
    index: number
    address: string
  }

  let { index, address }: Props = $props()

  const log = createLogger('address-link')

  function open(event: Event): void {
    event.stopPropagation()
    log.debug('open graph from address', { index })
    app.openGraph(index)
  }
</script>

<button
  type="button"
  class="address-link mono"
  title="Show the graph of what keeps it reachable"
  onclick={open}
  onkeydown={(event) => {
    if (event.key === 'Enter' || event.key === ' ') event.stopPropagation()
  }}>{address}</button
>

<style>
  .address-link {
    padding: 0 2px;
    border: 0;
    border-radius: var(--radius-sm);
    background: none;
    color: var(--accent);
    font-size: 0.92em;
    cursor: pointer;
    text-decoration: underline;
    text-decoration-color: color-mix(in srgb, var(--accent) 35%, transparent);
    text-underline-offset: 2px;
  }

  .address-link:hover {
    background: var(--accent-soft);
    text-decoration-color: var(--accent);
  }
</style>
