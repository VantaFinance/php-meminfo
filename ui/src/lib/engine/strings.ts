/**
 * Interned strings: each distinct value is stored once and referred to by a
 * small integer, so millions of items share a handful of class and type names.
 */
export class StringTable {
  private readonly ids = new Map<string, number>()
  private readonly values: string[] = []

  intern(value: string): number {
    let id = this.ids.get(value)
    if (id === undefined) {
      id = this.values.length
      this.values.push(value)
      this.ids.set(value, id)
    }
    return id
  }

  /** Id of an already interned value, or -1. */
  find(value: string): number {
    return this.ids.get(value) ?? -1
  }

  get(id: number): string {
    const value = this.values[id]
    if (value === undefined) {
      throw new RangeError(`no interned string with id ${id}`)
    }
    return value
  }

  get size(): number {
    return this.values.length
  }
}

/**
 * Byte-level front of a StringTable: a name already seen is found by its UTF-8
 * bytes, without decoding it to a JS string again. Class, type and property
 * names repeat on millions of items; decoding each occurrence dominated parsing.
 */
export class ByteInterner {
  private readonly buckets = new Map<number, Array<{ bytes: Uint8Array; id: number }>>()
  private readonly table: StringTable
  private readonly decode: (bytes: Uint8Array, length: number) => string

  constructor(table: StringTable, decode: (bytes: Uint8Array, length: number) => string) {
    this.table = table
    this.decode = decode
  }

  intern(bytes: Uint8Array, length: number): number {
    let hash = 0x811c9dc5
    for (let i = 0; i < length; i++) {
      hash = Math.imul(hash ^ bytes[i]!, 0x01000193)
    }
    let bucket = this.buckets.get(hash)
    if (bucket !== undefined) {
      for (const entry of bucket) {
        if (entry.bytes.length === length && sameBytes(entry.bytes, bytes, length)) {
          return entry.id
        }
      }
    } else {
      bucket = []
      this.buckets.set(hash, bucket)
    }
    const id = this.table.intern(this.decode(bytes, length))
    bucket.push({ bytes: bytes.slice(0, length), id })
    return id
  }
}

function sameBytes(a: Uint8Array, b: Uint8Array, length: number): boolean {
  for (let i = 0; i < length; i++) {
    if (a[i] !== b[i]) return false
  }
  return true
}
