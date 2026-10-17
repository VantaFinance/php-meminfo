import { allocate } from './columns'

/**
 * Item address → item index.
 *
 * Open addressing over an Int32Array that stores indices only (index + 1, 0 is
 * empty); the addresses themselves live in the store's two Uint32 columns and are
 * compared there. 4 bytes per slot at ≤ 0.7 load, instead of a Map of strings.
 */
export class AddressTable {
  private slots: Int32Array
  private mask: number
  private used = 0
  private hi: Uint32Array
  private lo: Uint32Array

  /**
   * `hi`/`lo` are the store's address columns. They are replaced when they grow
   * and when the store is frozen: call setColumns() each time, so that no
   * reference to a discarded column keeps it alive.
   */
  constructor(expected: number, hi: Uint32Array, lo: Uint32Array) {
    this.hi = hi
    this.lo = lo
    const size = AddressTable.sizeFor(expected)
    this.slots = allocate(Int32Array, size, 0)
    this.mask = size - 1
  }

  setColumns(hi: Uint32Array, lo: Uint32Array): void {
    this.hi = hi
    this.lo = lo
  }

  private static sizeFor(entries: number): number {
    let size = 1024
    while (size * 0.7 < entries) {
      size *= 2
    }
    return size
  }

  private static hash(hi: number, lo: number): number {
    // Addresses are 8-byte aligned: drop the low bits before mixing.
    let h = Math.imul((lo >>> 3) ^ Math.imul(hi, 0x9e3779b1), 0x85ebca6b)
    h ^= h >>> 13
    h = Math.imul(h, 0xc2b2ae35)
    return (h ^ (h >>> 16)) >>> 0
  }

  /** Index of the item with this address, or -1. */
  find(hi: number, lo: number): number {
    const his = this.hi
    const los = this.lo
    let slot = AddressTable.hash(hi, lo) & this.mask
    for (;;) {
      const entry = this.slots[slot]!
      if (entry === 0) {
        return -1
      }
      const index = entry - 1
      if (his[index] === hi && los[index] === lo) {
        return index
      }
      slot = (slot + 1) & this.mask
    }
  }

  /**
   * Record `index` for its address. Returns false when the address is already
   * present (the first occurrence keeps the slot).
   */
  insert(index: number, hi: number, lo: number, items: number): boolean {
    if ((this.used + 1) > this.slots.length * 0.7) {
      this.rehash(items)
    }
    const his = this.hi
    const los = this.lo
    let slot = AddressTable.hash(hi, lo) & this.mask
    for (;;) {
      const entry = this.slots[slot]!
      if (entry === 0) {
        this.slots[slot] = index + 1
        this.used++
        return true
      }
      const existing = entry - 1
      if (his[existing] === hi && los[existing] === lo) {
        return false
      }
      slot = (slot + 1) & this.mask
    }
  }

  private rehash(items: number): void {
    const old = this.slots
    const size = old.length * 2
    this.slots = allocate(Int32Array, size, items)
    this.mask = size - 1
    const his = this.hi
    const los = this.lo
    for (let i = 0; i < old.length; i++) {
      const entry = old[i]!
      if (entry === 0) {
        continue
      }
      const index = entry - 1
      let slot = AddressTable.hash(his[index]!, los[index]!) & this.mask
      while (this.slots[slot] !== 0) {
        slot = (slot + 1) & this.mask
      }
      this.slots[slot] = entry
    }
  }

  get byteLength(): number {
    return this.slots.byteLength
  }
}
