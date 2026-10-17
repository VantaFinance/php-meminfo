import { AddressTable } from './addressTable'
import { allocate, DumpTooLargeError, grow } from './columns'
import { StringTable } from './strings'

/** Item flags. */
export const FLAG_ROOT = 1
/** The item had a `children` key (arrays and objects, even when empty). */
export const FLAG_HAS_CHILDREN = 2

/** No interned string / no handle. */
export const NONE = -1

/** Numeric children keys are stored as `-(key + 1)`; interned names are ≥ 0. */
export function isNumericName(name: number): boolean {
  return name < 0
}

export interface DumpHeader {
  memory_usage?: number
  memory_usage_real?: number
  peak_memory_usage?: number
  peak_memory_usage_real?: number
  [field: string]: number | undefined
}

/** Plain view of one item, for details, tests and the parity checks. */
export interface ItemView {
  index: number
  address: string
  type: string
  size: number
  isRoot: boolean
  frame?: string
  symbol?: string
  className?: string
  handle?: number
  /** Present when the dump had a `children` key, in dump order, duplicates kept. */
  children?: Array<{ name: string; target: number; targetAddress: string }>
}

export interface StoreStats {
  items: number
  edges: number
  danglingEdges: number
  duplicateAddresses: number
  types: number
  classes: number
  frames: number
  symbols: number
  edgeNames: number
  bytes: number
}

/**
 * The parsed dump, column by column. Item i is described by the i-th entry of
 * every column; its children are edges childStart[i] … childStart[i + 1] - 1.
 */
export class DumpStore {
  constructor(
    readonly header: DumpHeader,
    readonly count: number,
    readonly addressHi: Uint32Array,
    readonly addressLo: Uint32Array,
    readonly type: Uint8Array,
    readonly flags: Uint8Array,
    readonly size: Float64Array,
    readonly classId: Int32Array,
    readonly frameId: Int32Array,
    readonly symbolId: Int32Array,
    readonly handle: Int32Array,
    readonly childStart: Uint32Array,
    readonly edgeCount: number,
    readonly edgeName: Int32Array,
    readonly edgeTarget: Int32Array,
    readonly types: StringTable,
    readonly classes: StringTable,
    readonly frames: StringTable,
    readonly symbols: StringTable,
    readonly edgeNames: StringTable,
    readonly addresses: AddressTable,
    readonly danglingEdges: number,
    readonly duplicateAddresses: number,
  ) {}

  address(index: number): string {
    return formatAddress(this.addressHi[index]!, this.addressLo[index]!)
  }

  /** Index of the item with this textual address, or -1. */
  findAddress(address: string): number {
    const parsed = parseAddressText(address)
    return parsed === null ? -1 : this.addresses.find(parsed[0], parsed[1])
  }

  typeName(index: number): string {
    return this.types.get(this.type[index]!)
  }

  isRoot(index: number): boolean {
    return (this.flags[index]! & FLAG_ROOT) !== 0
  }

  hasChildrenKey(index: number): boolean {
    return (this.flags[index]! & FLAG_HAS_CHILDREN) !== 0
  }

  className(index: number): string | undefined {
    const id = this.classId[index]!
    return id === NONE ? undefined : this.classes.get(id)
  }

  edgeNameText(edge: number): string {
    const name = this.edgeName[edge]!
    return isNumericName(name) ? String(-name - 1) : this.edgeNames.get(name)
  }

  item(index: number): ItemView {
    const view: ItemView = {
      index,
      address: this.address(index),
      type: this.typeName(index),
      size: this.size[index]!,
      isRoot: this.isRoot(index),
    }
    if (this.frameId[index] !== NONE) {
      view.frame = this.frames.get(this.frameId[index]!)
    }
    if (this.symbolId[index] !== NONE) {
      view.symbol = this.symbols.get(this.symbolId[index]!)
    }
    const className = this.className(index)
    if (className !== undefined) {
      view.className = className
    }
    if (this.handle[index] !== NONE) {
      view.handle = this.handle[index]!
    }
    if (this.hasChildrenKey(index)) {
      const children: NonNullable<ItemView['children']> = []
      for (let edge = this.childStart[index]!; edge < this.childStart[index + 1]!; edge++) {
        const target = this.edgeTarget[edge]!
        children.push({
          name: this.edgeNameText(edge),
          target,
          targetAddress: target < 0 ? '(missing)' : this.address(target),
        })
      }
      view.children = children
    }
    return view
  }

  stats(): StoreStats {
    const columns = [
      this.addressHi, this.addressLo, this.type, this.flags, this.size, this.classId, this.frameId,
      this.symbolId, this.handle, this.childStart, this.edgeName, this.edgeTarget,
    ]
    return {
      items: this.count,
      edges: this.edgeCount,
      danglingEdges: this.danglingEdges,
      duplicateAddresses: this.duplicateAddresses,
      types: this.types.size,
      classes: this.classes.size,
      frames: this.frames.size,
      symbols: this.symbols.size,
      edgeNames: this.edgeNames.size,
      bytes: columns.reduce((sum, column) => sum + column.byteLength, 0) + this.addresses.byteLength,
    }
  }
}

/** "0x7fb321a94050" → [hi, lo]; null when it is not a hex address. */
export function parseAddressText(text: string): [number, number] | null {
  if (!/^0x[0-9a-f]{1,16}$/i.test(text)) {
    return null
  }
  const digits = text.slice(2).padStart(16, '0')
  return [parseInt(digits.slice(0, 8), 16) >>> 0, parseInt(digits.slice(8), 16) >>> 0]
}

export function formatAddress(hi: number, lo: number): string {
  return hi === 0 ? '0x' + lo.toString(16) : '0x' + hi.toString(16) + lo.toString(16).padStart(8, '0')
}

/**
 * Mutable columns while the dump is being read. Children are recorded by
 * address and resolved to indices once every item is known: a child may come
 * before or after its parent.
 */
export class StoreBuilder {
  count = 0
  edgeCount = 0
  duplicateAddresses = 0
  header: DumpHeader = {}

  addressHi: Uint32Array
  addressLo: Uint32Array
  type: Uint8Array
  flags: Uint8Array
  size: Float64Array
  classId: Int32Array
  frameId: Int32Array
  symbolId: Int32Array
  handle: Int32Array
  childStart: Uint32Array

  edgeName: Int32Array
  edgeAddressHi: Uint32Array
  edgeAddressLo: Uint32Array

  readonly types = new StringTable()
  readonly classes = new StringTable()
  readonly frames = new StringTable()
  readonly symbols = new StringTable()
  readonly edgeNames = new StringTable()
  readonly addresses: AddressTable

  constructor(expectedItems: number) {
    const items = Math.max(1024, expectedItems)
    const edges = Math.max(1024, Math.ceil(expectedItems * 1.15))
    this.addressHi = allocate(Uint32Array, items, 0)
    this.addressLo = allocate(Uint32Array, items, 0)
    this.type = allocate(Uint8Array, items, 0)
    this.flags = allocate(Uint8Array, items, 0)
    this.size = allocate(Float64Array, items, 0)
    this.classId = allocate(Int32Array, items, 0).fill(NONE)
    this.frameId = allocate(Int32Array, items, 0).fill(NONE)
    this.symbolId = allocate(Int32Array, items, 0).fill(NONE)
    this.handle = allocate(Int32Array, items, 0).fill(NONE)
    this.childStart = allocate(Uint32Array, items + 1, 0)
    this.edgeName = allocate(Int32Array, edges, 0)
    this.edgeAddressHi = allocate(Uint32Array, edges, 0)
    this.edgeAddressLo = allocate(Uint32Array, edges, 0)
    this.addresses = new AddressTable(items, this.addressHi, this.addressLo)
  }

  /** Start a new item; returns its index. */
  beginItem(hi: number, lo: number): number {
    const index = this.count
    if (index >= this.addressHi.length) {
      this.growItems(index + 1)
    }
    this.addressHi[index] = hi
    this.addressLo[index] = lo
    this.childStart[index] = this.edgeCount
    this.count = index + 1
    if (!this.addresses.insert(index, hi, lo, this.count)) {
      this.duplicateAddresses++
    }
    return index
  }

  addEdge(name: number, hi: number, lo: number): void {
    const edge = this.edgeCount
    if (edge >= this.edgeName.length) {
      const items = this.count
      this.edgeName = grow(this.edgeName, edge + 1, Int32Array, items)
      this.edgeAddressHi = grow(this.edgeAddressHi, edge + 1, Uint32Array, items)
      this.edgeAddressLo = grow(this.edgeAddressLo, edge + 1, Uint32Array, items)
    }
    this.edgeName[edge] = name
    this.edgeAddressHi[edge] = hi
    this.edgeAddressLo[edge] = lo
    this.edgeCount = edge + 1
  }

  private growItems(needed: number): void {
    const items = this.count
    const oldLength = this.classId.length
    this.addressHi = grow(this.addressHi, needed, Uint32Array, items)
    this.addressLo = grow(this.addressLo, needed, Uint32Array, items)
    this.type = grow(this.type, needed, Uint8Array, items)
    this.flags = grow(this.flags, needed, Uint8Array, items)
    this.size = grow(this.size, needed, Float64Array, items)
    this.classId = grow(this.classId, needed, Int32Array, items)
    this.frameId = grow(this.frameId, needed, Int32Array, items)
    this.symbolId = grow(this.symbolId, needed, Int32Array, items)
    this.handle = grow(this.handle, needed, Int32Array, items)
    // Sized from the address column, not from `needed`: it is one longer than the
    // other columns and would otherwise fall behind them as they grow.
    this.childStart = grow(this.childStart, this.addressHi.length + 1, Uint32Array, items)
    this.addresses.setColumns(this.addressHi, this.addressLo)
    for (const column of [this.classId, this.frameId, this.symbolId, this.handle]) {
      column.fill(NONE, oldLength)
    }
  }

  /** Resolve children to indices and freeze the columns at their final length. */
  finish(): DumpStore {
    try {
      return this.freeze()
    } catch (error) {
      if (error instanceof RangeError) {
        throw new DumpTooLargeError(this.count, error)
      }
      throw error
    }
  }

  private freeze(): DumpStore {
    const count = this.count
    const edges = this.edgeCount
    this.childStart = grow(this.childStart, count + 1, Uint32Array, count)
    this.childStart[count] = edges

    const edgeTarget = allocate(Int32Array, edges, count)
    let dangling = 0
    for (let edge = 0; edge < edges; edge++) {
      const target = this.addresses.find(this.edgeAddressHi[edge]!, this.edgeAddressLo[edge]!)
      if (target < 0) {
        dangling++
      }
      edgeTarget[edge] = target
    }

    // Exact-length copies: the slack of the growable columns is released with
    // the builder, and so are the child addresses, needed no longer.
    const addressHi = this.addressHi.slice(0, count)
    const addressLo = this.addressLo.slice(0, count)
    this.addresses.setColumns(addressHi, addressLo)

    const store = new DumpStore(
      this.header,
      count,
      addressHi,
      addressLo,
      this.type.slice(0, count),
      this.flags.slice(0, count),
      this.size.slice(0, count),
      this.classId.slice(0, count),
      this.frameId.slice(0, count),
      this.symbolId.slice(0, count),
      this.handle.slice(0, count),
      this.childStart.slice(0, count + 1),
      edges,
      this.edgeName.slice(0, edges),
      edgeTarget,
      this.types,
      this.classes,
      this.frames,
      this.symbols,
      this.edgeNames,
      this.addresses,
      dangling,
      this.duplicateAddresses,
    )
    return store
  }
}
