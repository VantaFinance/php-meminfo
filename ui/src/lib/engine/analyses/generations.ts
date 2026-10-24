import { createLogger } from '../../log'
import { NONE, type DumpStore } from '../store'

const log = createLogger('generations')

/**
 * Objects by generation (meminfo_mark()): how many instances of each class
 * were first seen at each mark. A class whose old generations stay populated
 * dump after dump keeps objects it should have let go — the survivors YourKit
 * and JProfiler point at. The newest generation is what was created since the
 * last mark, most of it normally short-lived.
 */
export interface GenerationRow {
  key: string
  /** Instances per generation, in the order of GenerationTable.generations. */
  counts: number[]
  /** Instances older than the newest generation. */
  survivors: number
}

export interface GenerationTable {
  generations: number[]
  rows: GenerationRow[]
}

export function generationTable(store: DumpStore, limit = 200): GenerationTable | null {
  const generation = store.generation
  if (generation === null) return null

  const present = new Set<number>()
  const byClass = new Map<number, Map<number, number>>()
  for (let i = 0; i < store.count; i++) {
    const g = generation[i]!
    const classId = store.classId[i]!
    if (g === NONE || classId === NONE) continue
    present.add(g)
    let counts = byClass.get(classId)
    if (counts === undefined) {
      counts = new Map()
      byClass.set(classId, counts)
    }
    counts.set(g, (counts.get(g) ?? 0) + 1)
  }

  const generations = [...present].sort((a, b) => a - b)
  const newest = generations.at(-1)
  const rows: GenerationRow[] = []
  for (const [classId, counts] of byClass) {
    const row = generations.map((g) => counts.get(g) ?? 0)
    rows.push({
      key: store.classes.get(classId),
      counts: row,
      survivors: generations.reduce((sum, g, at) => (g === newest ? sum : sum + row[at]!), 0),
    })
  }
  rows.sort((a, b) => b.survivors - a.survivors || a.key.localeCompare(b.key))
  log.info('generations', { generations: generations.length, classes: rows.length })
  return { generations, rows: rows.slice(0, limit) }
}
