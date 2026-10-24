import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import { computeDominators } from './dominators'
import { buildPredecessors } from './graph'
import { allocationSites, packageOf } from './sites'

describe('allocationSites', () => {
  it('maps a site to its Composer package', () => {
    expect(packageOf('/app/vendor/acme/http/src/Client.php:12')).toBe('acme/http')
    expect(packageOf('/app/src/Cache.php:3')).toBe('(application)')
  })

  it('is null without tracking, and groups objects by site and package', () => {
    const parse = (text: string) => {
      const parser = new DumpParser()
      parser.push(new TextEncoder().encode(text))
      return parser.finish()
    }
    expect(allocationSites(parse('{"items":{}}'), null)).toBeNull()

    const store = parse(
      '{"items":{"0x10":{"type":"array","size":"56","symbol_name":"all","is_root":true,"frame":"<GLOBAL>","children":{"0":"0x20","1":"0x30","2":"0x40"}},' +
        '"0x20":{"type":"object","size":"100","is_root":false,"class":"Row","site":"/app/vendor/acme/db/src/Fetch.php:9"},' +
        '"0x30":{"type":"object","size":"100","is_root":false,"class":"Row","site":"/app/vendor/acme/db/src/Fetch.php:9"},' +
        '"0x40":{"type":"object","size":"50","is_root":false,"class":"Other"}}}',
    )
    const report = allocationSites(store, computeDominators(store, buildPredecessors(store)))!
    expect(report).toMatchObject({ tracked: 2, objects: 3 })
    expect(report.sites).toEqual([{ key: '/app/vendor/acme/db/src/Fetch.php:9', objects: 2, retained: 200 }])
    expect(report.packages).toEqual([{ key: 'acme/db', objects: 2, retained: 200 }])
  })
})
