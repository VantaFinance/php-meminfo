import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import { generationTable } from './generations'

function parse(text: string) {
  const parser = new DumpParser()
  parser.push(new TextEncoder().encode(text))
  return parser.finish()
}

describe('generationTable', () => {
  it('is null for a dump without generations', () => {
    expect(generationTable(parse('{"items":{"0x10":{"type":"object","size":"72","is_root":true,"frame":"<GLOBAL>","symbol_name":"a","class":"A"}}}'))).toBeNull()
  })

  it('counts instances per class and generation, survivors first', () => {
    const items = [
      ['0x10', 'Cache', 1],
      ['0x20', 'Cache', 2],
      ['0x30', 'Request', 3],
      ['0x40', 'Request', 3],
      ['0x50', 'Request', 1],
      ['0x60', 'Temp', 3],
    ]
      .map(([address, className, generation]) => `"${address}":{"type":"object","size":"72","is_root":false,"class":"${className}","generation":${generation}}`)
      .join(',')
    const table = generationTable(parse(`{"items":{${items},"0x70":{"type":"int","size":"0","is_root":true,"frame":"<GLOBAL>","symbol_name":"n"}}}`))!

    expect(table.generations).toEqual([1, 2, 3])
    expect(table.rows.map((row) => [row.key, row.counts, row.survivors])).toEqual([
      ['Cache', [1, 1, 0], 2],
      ['Request', [1, 0, 2], 1],
      ['Temp', [0, 0, 1], 0],
    ])
  })
})
