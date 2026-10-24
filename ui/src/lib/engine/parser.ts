import { createLogger } from '../log'
import { FLAG_HAS_CHILDREN, FLAG_ROOT, StoreBuilder, type DumpStore } from './store'
import { ByteInterner } from './strings'

/**
 * Streaming parser for meminfo dumps.
 *
 * A byte-level JSON tokenizer fed chunks of any size (browsers deliver 64 KiB to
 * 2 MiB), with its state carried across chunk boundaries — a string, an escape
 * or a number may be split anywhere. The dump's shape is applied on top of the
 * token stream: `header`, `items`, the fields of an item and its `children`.
 * Field order is not relied upon and unknown fields are skipped.
 *
 * Nothing is ever held as one string: `Blob.text()` silently returns "" above
 * 512 MiB and `JSON.parse` cannot go there at all. Only the bytes of the string
 * being read are buffered, and only names are decoded to JS strings.
 */

const log = createLogger('parser')

export class DumpParseError extends Error {
  readonly offset: number

  constructor(message: string, offset: number) {
    super(`${message} at byte ${offset}`)
    this.name = 'DumpParseError'
    this.offset = offset
  }
}

// Containers on the stack.
const CTX_ROOT = 0
const CTX_TOP = 1
const CTX_HEADER = 2
const CTX_ITEMS = 3
const CTX_ITEM = 4
const CTX_CHILDREN = 5
const CTX_SKIP_OBJECT = 6
const CTX_SKIP_ARRAY = 7

// Tokenizer states.
const S_VALUE = 0
const S_STRING = 1
const S_ESCAPE = 2
const S_UNICODE = 3
const S_NUMBER = 4
const S_LITERAL = 5

// Item fields.
const F_UNKNOWN = 0
const F_TYPE = 1
const F_SIZE = 2
const F_SYMBOL = 3
const F_IS_ROOT = 4
const F_FRAME = 5
const F_CLASS = 6
const F_HANDLE = 7
const F_CHILDREN = 8
const F_FUNCTION = 9
const F_DECLARED_AT = 10
const F_LINE = 11
const F_RESOURCE_TYPE = 12
const F_CAPACITY = 13
const F_USED = 14
const F_HASH = 15
const F_GENERATION = 16
const F_SITE = 17

const encoder = new TextEncoder()
const FIELDS: Array<[Uint8Array, number]> = [
  [encoder.encode('type'), F_TYPE],
  [encoder.encode('size'), F_SIZE],
  [encoder.encode('symbol_name'), F_SYMBOL],
  [encoder.encode('is_root'), F_IS_ROOT],
  [encoder.encode('frame'), F_FRAME],
  [encoder.encode('class'), F_CLASS],
  [encoder.encode('object_handle'), F_HANDLE],
  [encoder.encode('children'), F_CHILDREN],
  [encoder.encode('function'), F_FUNCTION],
  [encoder.encode('declared_at'), F_DECLARED_AT],
  [encoder.encode('line'), F_LINE],
  [encoder.encode('resource_type'), F_RESOURCE_TYPE],
  [encoder.encode('capacity'), F_CAPACITY],
  [encoder.encode('used'), F_USED],
  [encoder.encode('hash'), F_HASH],
  [encoder.encode('generation'), F_GENERATION],
  [encoder.encode('site'), F_SITE],
]
const KEY_HEADER = encoder.encode('header')
const KEY_ITEMS = encoder.encode('items')

const decoder = new TextDecoder('utf-8', { fatal: false })

const LITERAL_TRUE = 1
const LITERAL_FALSE = 2
const LITERAL_NULL = 3
const LITERALS = ['', 'true', 'false', 'null']

/** Invalid UTF-8 (written raw by the extension) becomes U+FFFD. */
function decodeBytes(s: Uint8Array, length: number): string {
  if (length <= 64) {
    let ascii = true
    for (let i = 0; i < length; i++) {
      if (s[i]! >= 0x80) {
        ascii = false
        break
      }
    }
    if (ascii) {
      return String.fromCharCode.apply(null, s.subarray(0, length) as unknown as number[])
    }
  }
  return decoder.decode(s.subarray(0, length))
}

function hexValue(byte: number): number {
  if (byte >= 0x30 && byte <= 0x39) return byte - 0x30
  if (byte >= 0x61 && byte <= 0x66) return byte - 0x57
  if (byte >= 0x41 && byte <= 0x46) return byte - 0x37
  return -1
}

export interface ParseProgress {
  bytes: number
  items: number
}

export class DumpParser {
  private readonly builder: StoreBuilder

  private stack: number[] = [CTX_ROOT]
  private expectKey = false

  private state = S_VALUE
  private string = new Uint8Array(256)
  private stringLength = 0
  private unicodeValue = 0
  private unicodeDigits = 0
  private highSurrogate = 0
  private number = ''
  private literal = 0
  private literalPosition = 0

  /** Key that the next value belongs to, per container kind. */
  private topKey = 0 // 1 header, 2 items, 0 other
  private headerKey = ''
  private field = F_UNKNOWN
  private item = -1
  private edgeName = 0

  private addressHi = 0
  private addressLo = 0

  private offset = 0
  private readonly unknownFields = new Map<string, number>()

  private readonly typeNames: ByteInterner
  private readonly classNames: ByteInterner
  private readonly frameNames: ByteInterner
  private readonly childNames: ByteInterner

  constructor(expectedBytes = 0) {
    // Real dumps take 150–167 bytes per item on disk.
    this.builder = new StoreBuilder(Math.ceil(expectedBytes / 130))
    this.typeNames = new ByteInterner(this.builder.types, decodeBytes)
    this.classNames = new ByteInterner(this.builder.classes, decodeBytes)
    this.frameNames = new ByteInterner(this.builder.frames, decodeBytes)
    this.childNames = new ByteInterner(this.builder.edgeNames, decodeBytes)
  }

  get progress(): ParseProgress {
    return { bytes: this.offset, items: this.builder.count }
  }

  push(chunk: Uint8Array): void {
    const length = chunk.length
    let i = 0

    while (i < length) {
      const byte = chunk[i]!

      switch (this.state) {
        case S_STRING: {
          // Copy a run of plain bytes at once.
          let end = i
          while (end < length) {
            const b = chunk[end]!
            if (b === 0x22 || b === 0x5c) break
            end++
          }
          if (end > i) {
            this.appendBytes(chunk, i, end)
            i = end
            continue
          }
          if (byte === 0x22) {
            this.flushSurrogate()
            this.state = S_VALUE
            this.onString()
          } else {
            this.state = S_ESCAPE
          }
          i++
          continue
        }

        case S_ESCAPE: {
          this.state = S_STRING
          switch (byte) {
            case 0x22: this.appendCodePoint(0x22); break
            case 0x5c: this.appendCodePoint(0x5c); break
            case 0x2f: this.appendCodePoint(0x2f); break
            case 0x62: this.appendCodePoint(0x08); break
            case 0x66: this.appendCodePoint(0x0c); break
            case 0x6e: this.appendCodePoint(0x0a); break
            case 0x72: this.appendCodePoint(0x0d); break
            case 0x74: this.appendCodePoint(0x09); break
            case 0x75:
              this.state = S_UNICODE
              this.unicodeValue = 0
              this.unicodeDigits = 0
              break
            default:
              throw new DumpParseError(`invalid escape \\${String.fromCharCode(byte)}`, this.offset + i)
          }
          i++
          continue
        }

        case S_UNICODE: {
          const value = hexValue(byte)
          if (value < 0) {
            throw new DumpParseError('invalid \\u escape', this.offset + i)
          }
          this.unicodeValue = this.unicodeValue * 16 + value
          if (++this.unicodeDigits === 4) {
            this.state = S_STRING
            this.appendUnicodeEscape(this.unicodeValue)
          }
          i++
          continue
        }

        case S_NUMBER: {
          if ((byte >= 0x30 && byte <= 0x39) || byte === 0x2d || byte === 0x2b || byte === 0x2e || byte === 0x65 || byte === 0x45) {
            this.number += String.fromCharCode(byte)
            i++
            continue
          }
          this.state = S_VALUE
          this.onNumber()
          continue // the terminator is a token of its own
        }

        case S_LITERAL: {
          const text = LITERALS[this.literal]!
          if (byte !== text.charCodeAt(this.literalPosition)) {
            throw new DumpParseError('invalid literal', this.offset + i)
          }
          if (++this.literalPosition === text.length) {
            this.state = S_VALUE
            this.onLiteral(this.literal)
          }
          i++
          continue
        }

        default: {
          switch (byte) {
            case 0x20: case 0x0a: case 0x0d: case 0x09:
              break
            case 0x22:
              this.state = S_STRING
              this.stringLength = 0
              this.highSurrogate = 0
              break
            case 0x7b: // {
              this.openObject(this.offset + i)
              break
            case 0x7d: // }
              this.close(false, this.offset + i)
              break
            case 0x5b: // [
              this.openArray(this.offset + i)
              break
            case 0x5d: // ]
              this.close(true, this.offset + i)
              break
            case 0x3a: // :
              this.expectKey = false
              break
            case 0x2c: // ,
              this.expectKey = this.top() !== CTX_SKIP_ARRAY
              break
            case 0x74: this.startLiteral(LITERAL_TRUE); break
            case 0x66: this.startLiteral(LITERAL_FALSE); break
            case 0x6e: this.startLiteral(LITERAL_NULL); break
            default:
              if ((byte >= 0x30 && byte <= 0x39) || byte === 0x2d) {
                this.state = S_NUMBER
                this.number = String.fromCharCode(byte)
                break
              }
              throw new DumpParseError(`unexpected byte 0x${byte.toString(16)}`, this.offset + i)
          }
          i++
        }
      }
    }

    this.offset += length
  }

  /** End of input: check the document is complete and resolve the children. */
  finish(): DumpStore {
    if (this.state === S_NUMBER) {
      this.state = S_VALUE
      this.onNumber()
    }
    if (this.state !== S_VALUE || this.stack.length !== 1) {
      throw new DumpParseError('unexpected end of the dump', this.offset)
    }
    if (this.unknownFields.size > 0) {
      log.warn('skipped unknown item fields', Object.fromEntries(this.unknownFields))
    }
    const store = this.builder.finish()
    if (store.danglingEdges > 0) {
      log.warn('children point at items missing from the dump', { dangling: store.danglingEdges })
    }
    if (store.duplicateAddresses > 0) {
      log.warn('items with duplicate addresses; the first one is used', { duplicates: store.duplicateAddresses })
    }
    return store
  }

  /* --- tokens -------------------------------------------------------------- */

  private top(): number {
    return this.stack[this.stack.length - 1]!
  }

  private startLiteral(literal: number): void {
    this.state = S_LITERAL
    this.literal = literal
    this.literalPosition = 1
  }

  private openObject(offset: number): void {
    const parent = this.top()
    let ctx = CTX_SKIP_OBJECT
    if (this.expectKey) {
      throw new DumpParseError('an object cannot be a key', offset)
    }
    switch (parent) {
      case CTX_ROOT:
        ctx = CTX_TOP
        break
      case CTX_TOP:
        ctx = this.topKey === 1 ? CTX_HEADER : this.topKey === 2 ? CTX_ITEMS : CTX_SKIP_OBJECT
        break
      case CTX_ITEMS:
        ctx = CTX_ITEM
        this.field = F_UNKNOWN
        break
      case CTX_ITEM:
        if (this.field === F_CHILDREN) {
          ctx = CTX_CHILDREN
          this.builder.flags[this.item] = this.builder.flags[this.item]! | FLAG_HAS_CHILDREN
        }
        break
    }
    this.stack.push(ctx)
    this.expectKey = true
  }

  private openArray(offset: number): void {
    if (this.expectKey) {
      throw new DumpParseError('an array cannot be a key', offset)
    }
    this.stack.push(CTX_SKIP_ARRAY)
    this.expectKey = false
  }

  private close(isArray: boolean, offset: number): void {
    const ctx = this.top()
    if (this.stack.length === 1 || (ctx === CTX_SKIP_ARRAY) !== isArray) {
      throw new DumpParseError('unbalanced bracket', offset)
    }
    this.stack.pop()
    this.expectKey = false
  }

  private onString(): void {
    const ctx = this.top()
    if (this.expectKey) {
      this.onKey(ctx)
      return
    }
    switch (ctx) {
      case CTX_ITEM:
        this.onItemString()
        return
      case CTX_CHILDREN: {
        if (!this.parseAddress()) {
          throw new DumpParseError('child is not an address', this.offset)
        }
        this.builder.addEdge(this.edgeName, this.addressHi, this.addressLo)
        return
      }
      case CTX_HEADER:
        this.builder.header[this.headerKey] = this.decodeString()
        return
      default:
        // values of unknown fields: not needed
        return
    }
  }

  private onKey(ctx: number): void {
    switch (ctx) {
      case CTX_TOP:
        this.topKey = this.stringEquals(KEY_HEADER) ? 1 : this.stringEquals(KEY_ITEMS) ? 2 : 0
        return
      case CTX_HEADER:
        this.headerKey = this.decodeString()
        return
      case CTX_ITEMS: {
        if (!this.parseAddress()) {
          throw new DumpParseError(`item key is not an address: ${this.decodeString().slice(0, 40)}`, this.offset)
        }
        this.item = this.builder.beginItem(this.addressHi, this.addressLo)
        if ((this.builder.count & 0x3ffff) === 0) {
          log.debug('items read', { items: this.builder.count, bytes: this.offset })
        }
        return
      }
      case CTX_ITEM:
        this.field = this.matchField()
        if (this.field === F_UNKNOWN) {
          const name = this.decodeString()
          this.unknownFields.set(name, (this.unknownFields.get(name) ?? 0) + 1)
        }
        return
      case CTX_CHILDREN:
        this.edgeName = this.childName()
        return
      default:
        return
    }
  }

  private onItemString(): void {
    const b = this.builder
    const item = this.item
    switch (this.field) {
      case F_TYPE: {
        const id = this.typeNames.intern(this.string, this.stringLength)
        if (id > 255) {
          throw new DumpParseError('more than 256 distinct item types', this.offset)
        }
        b.type[item] = id
        return
      }
      case F_SIZE:
        b.size[item] = this.parseDecimal()
        return
      case F_SYMBOL:
        b.symbolId[item] = b.symbols.intern(this.decodeString())
        return
      case F_FRAME:
        b.frameId[item] = this.frameNames.intern(this.string, this.stringLength)
        return
      case F_CLASS:
        b.classId[item] = this.classNames.intern(this.string, this.stringLength)
        return
      case F_FUNCTION:
        b.extra(item).function = this.decodeString()
        return
      case F_DECLARED_AT:
        b.extra(item).declaredAt = this.decodeString()
        return
      case F_RESOURCE_TYPE:
        b.extra(item).resourceType = this.decodeString()
        return
      case F_HASH:
        b.extra(item).hash = this.decodeString()
        return
      case F_SITE:
        b.setSite(item, this.decodeString())
        return
      case F_HANDLE:
        b.handle[item] = this.parseDecimal() | 0
        return
      default:
        return
    }
  }

  private onNumber(): void {
    const value = Number(this.number)
    if (Number.isNaN(value)) {
      throw new DumpParseError(`invalid number ${this.number}`, this.offset)
    }
    const ctx = this.top()
    if (ctx === CTX_HEADER) {
      this.builder.header[this.headerKey] = value
    } else if (ctx === CTX_ITEM) {
      // Sizes and handles are strings in every dump so far; accept numbers too.
      switch (this.field) {
        case F_SIZE:
          this.builder.size[this.item] = value
          return
        case F_HANDLE:
          this.builder.handle[this.item] = value | 0
          return
        case F_LINE:
          this.builder.extra(this.item).line = value
          return
        case F_CAPACITY:
          this.builder.extra(this.item).capacity = value
          return
        case F_USED:
          this.builder.extra(this.item).used = value
          return
        case F_GENERATION:
          this.builder.setGeneration(this.item, value | 0)
          return
      }
    }
  }

  private onLiteral(literal: number): void {
    if (this.top() === CTX_ITEM && this.field === F_IS_ROOT) {
      const flags = this.builder.flags
      flags[this.item] = literal === LITERAL_TRUE ? flags[this.item]! | FLAG_ROOT : flags[this.item]! & ~FLAG_ROOT
    }
  }

  /* --- the string being read ---------------------------------------------- */

  private ensureString(extra: number): void {
    const needed = this.stringLength + extra
    if (needed > this.string.length) {
      let size = this.string.length * 2
      while (size < needed) size *= 2
      const next = new Uint8Array(size)
      next.set(this.string.subarray(0, this.stringLength))
      this.string = next
    }
  }

  private appendBytes(chunk: Uint8Array, start: number, end: number): void {
    this.flushSurrogate()
    const length = end - start
    this.ensureString(length)
    const s = this.string
    let n = this.stringLength
    if (length < 64) {
      // A loop beats creating a subarray view for the short names a dump is made of.
      for (let i = start; i < end; i++) {
        s[n++] = chunk[i]!
      }
    } else {
      s.set(chunk.subarray(start, end), n)
      n += length
    }
    this.stringLength = n
  }

  private appendUnicodeEscape(code: number): void {
    if (code >= 0xd800 && code <= 0xdbff) {
      this.flushSurrogate()
      this.highSurrogate = code
      return
    }
    if (code >= 0xdc00 && code <= 0xdfff) {
      if (this.highSurrogate !== 0) {
        const combined = 0x10000 + ((this.highSurrogate - 0xd800) << 10) + (code - 0xdc00)
        this.highSurrogate = 0
        this.appendCodePoint(combined)
      } else {
        this.appendCodePoint(0xfffd)
      }
      return
    }
    this.appendCodePoint(code)
  }

  /** A high surrogate not followed by a low one is replaced. */
  private flushSurrogate(): void {
    if (this.highSurrogate !== 0) {
      this.highSurrogate = 0
      this.writeCodePoint(0xfffd)
    }
  }

  private appendCodePoint(code: number): void {
    this.flushSurrogate()
    this.writeCodePoint(code)
  }

  private writeCodePoint(code: number): void {
    this.ensureString(4)
    const s = this.string
    let n = this.stringLength
    if (code < 0x80) {
      s[n++] = code
    } else if (code < 0x800) {
      s[n++] = 0xc0 | (code >> 6)
      s[n++] = 0x80 | (code & 0x3f)
    } else if (code < 0x10000) {
      s[n++] = 0xe0 | (code >> 12)
      s[n++] = 0x80 | ((code >> 6) & 0x3f)
      s[n++] = 0x80 | (code & 0x3f)
    } else {
      s[n++] = 0xf0 | (code >> 18)
      s[n++] = 0x80 | ((code >> 12) & 0x3f)
      s[n++] = 0x80 | ((code >> 6) & 0x3f)
      s[n++] = 0x80 | (code & 0x3f)
    }
    this.stringLength = n
  }

  private stringEquals(bytes: Uint8Array): boolean {
    if (this.stringLength !== bytes.length) return false
    for (let i = 0; i < bytes.length; i++) {
      if (this.string[i] !== bytes[i]) return false
    }
    return true
  }

  private matchField(): number {
    for (const [bytes, field] of FIELDS) {
      if (this.stringEquals(bytes)) return field
    }
    return F_UNKNOWN
  }

  private decodeString(): string {
    return decodeBytes(this.string, this.stringLength)
  }

  /** "0x…" → this.addressHi / this.addressLo; false when it is not an address. */
  private parseAddress(): boolean {
    const s = this.string
    const length = this.stringLength
    if (length < 3 || length > 18 || s[0] !== 0x30 || (s[1] !== 0x78 && s[1] !== 0x58)) {
      return false
    }
    let hi = 0
    let lo = 0
    for (let i = 2; i < length; i++) {
      const value = hexValue(s[i]!)
      if (value < 0) return false
      if (length - i > 8) {
        hi = hi * 16 + value
      } else {
        lo = lo * 16 + value
      }
    }
    this.addressHi = hi >>> 0
    this.addressLo = lo >>> 0
    return true
  }

  /** Digits of a size or handle string. */
  private parseDecimal(): number {
    const s = this.string
    let value = 0
    let i = 0
    let negative = false
    if (this.stringLength > 0 && s[0] === 0x2d) {
      negative = true
      i = 1
    }
    for (; i < this.stringLength; i++) {
      const digit = s[i]! - 0x30
      if (digit < 0 || digit > 9) {
        throw new DumpParseError(`not a number: ${this.decodeString().slice(0, 40)}`, this.offset)
      }
      value = value * 10 + digit
    }
    return negative ? -value : value
  }

  /**
   * Children key → edge name. Canonical non-negative decimals that fit 31 bits
   * are stored inline as -(key + 1); anything else ("-3", "0123",
   * "9223372036854775807", property names) is interned.
   */
  private childName(): number {
    const s = this.string
    const length = this.stringLength
    if (length >= 1 && length <= 10 && !(length > 1 && s[0] === 0x30)) {
      let value = 0
      let numeric = true
      for (let i = 0; i < length; i++) {
        const digit = s[i]! - 0x30
        if (digit < 0 || digit > 9) {
          numeric = false
          break
        }
        value = value * 10 + digit
      }
      if (numeric && value <= 0x7ffffffe) {
        return -(value + 1)
      }
    }
    return this.childNames.intern(this.string, this.stringLength)
  }
}

export interface ParseOptions {
  /** File size, used to pre-size the columns. */
  expectedBytes?: number
  onProgress?: (progress: ParseProgress) => void
  signal?: AbortSignal
}

/** Read a whole dump from a byte stream. */
export async function parseDumpStream(stream: ReadableStream<Uint8Array>, options: ParseOptions = {}): Promise<DumpStore> {
  const parser = new DumpParser(options.expectedBytes ?? 0)
  const reader = stream.getReader()
  const started = performance.now()

  try {
    for (;;) {
      if (options.signal?.aborted) {
        throw new DOMException('parsing was cancelled', 'AbortError')
      }
      const { done, value } = await reader.read()
      if (done) break
      parser.push(value)
      options.onProgress?.(parser.progress)
    }
  } finally {
    reader.releaseLock()
  }

  const store = parser.finish()
  const stats = store.stats()
  const ms = Math.round(performance.now() - started)
  log.info('dump parsed', {
    items: stats.items,
    edges: stats.edges,
    names: stats.classes + stats.frames + stats.symbols + stats.edgeNames,
    ms,
    bytesPerItem: stats.items > 0 ? Math.round(stats.bytes / stats.items) : 0,
  })
  return store
}
