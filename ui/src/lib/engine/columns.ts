/**
 * Growable typed-array columns.
 *
 * Growth is ×1.5 rather than ×2: the old and the new buffer coexist while the
 * data is copied, and on a dump of millions of items that transient peak is what
 * decides whether the worker survives.
 */

export class DumpTooLargeError extends Error {
  readonly items: number

  constructor(items: number, cause?: unknown) {
    super(`the dump is too large for this browser — ${items.toLocaleString('en')} items were read before memory ran out`)
    this.name = 'DumpTooLargeError'
    this.items = items
    if (cause !== undefined) {
      this.cause = cause
    }
  }
}

type TypedArray = Uint8Array | Int32Array | Uint32Array | Float64Array

type TypedArrayConstructor<T extends TypedArray> = { new (length: number): T }

/** Next capacity ≥ needed, growing by half. */
export function nextCapacity(current: number, needed: number): number {
  let capacity = Math.max(current, 16)
  while (capacity < needed) {
    capacity = Math.ceil(capacity * 1.5)
  }
  return capacity
}

/**
 * Copy `array` into a larger one. A failed allocation is the one out-of-memory
 * condition a browser lets us catch, and is reported as DumpTooLargeError.
 */
export function grow<T extends TypedArray>(array: T, needed: number, ctor: TypedArrayConstructor<T>, items: number): T {
  if (array.length >= needed) {
    return array
  }
  let next: T
  try {
    next = new ctor(nextCapacity(array.length, needed))
  } catch (error) {
    if (error instanceof RangeError) {
      throw new DumpTooLargeError(items, error)
    }
    throw error
  }
  next.set(array as never)
  return next
}

/** Allocate, mapping an allocation failure to DumpTooLargeError. */
export function allocate<T extends TypedArray>(ctor: TypedArrayConstructor<T>, length: number, items: number): T {
  try {
    return new ctor(length)
  } catch (error) {
    if (error instanceof RangeError) {
      throw new DumpTooLargeError(items, error)
    }
    throw error
  }
}
