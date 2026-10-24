import { createLogger } from '../../log'
import { NONE, type DumpStore } from '../store'
import type { Dominators } from './dominators'

const log = createLogger('sites')

/**
 * Where the objects of a dump were created (meminfo.track_allocations): count
 * and retained size per "file:line", and per Composer package — the code that
 * creates what leaks, rather than the class it is (JFR's allocation stack,
 * Ruby's trace_object_allocations).
 */
export interface SiteRow {
  key: string
  objects: number
  retained: number
}

export interface SitesReport {
  /** Objects with a site / objects in the dump. */
  tracked: number
  objects: number
  sites: SiteRow[]
  packages: SiteRow[]
}

/** "…/vendor/acme/http/src/Client.php:12" → "acme/http"; the application otherwise. */
export function packageOf(site: string): string {
  const match = /(?:^|\/)vendor\/([^/]+\/[^/]+)\//.exec(site)
  return match === null ? '(application)' : match[1]!
}

export function allocationSites(store: DumpStore, dominators: Dominators | null, limit = 200): SitesReport | null {
  const siteId = store.siteId
  if (siteId === null) return null

  const bySite = new Map<number, SiteRow>()
  let objects = 0
  let tracked = 0
  for (let i = 0; i < store.count; i++) {
    if (store.classId[i] === NONE) continue
    objects++
    const site = siteId[i]!
    if (site === NONE) continue
    tracked++
    const retained = dominators !== null && dominators.idom[i] !== -1 ? dominators.retained[i]! : store.size[i]!
    const row = bySite.get(site) ?? { key: store.sites.get(site), objects: 0, retained: 0 }
    row.objects++
    row.retained += retained
    bySite.set(site, row)
  }

  const byPackage = new Map<string, SiteRow>()
  for (const row of bySite.values()) {
    const key = packageOf(row.key)
    const entry = byPackage.get(key) ?? { key, objects: 0, retained: 0 }
    entry.objects += row.objects
    entry.retained += row.retained
    byPackage.set(key, entry)
  }

  const order = (a: SiteRow, b: SiteRow): number => b.retained - a.retained || b.objects - a.objects
  const report = {
    tracked,
    objects,
    sites: [...bySite.values()].sort(order).slice(0, limit),
    packages: [...byPackage.values()].sort(order).slice(0, limit),
  }
  log.info('allocation sites', { sites: bySite.size, packages: byPackage.size, tracked })
  return report
}
