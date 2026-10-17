/**
 * The k best (score, index) pairs without sorting everything: a bounded
 * min-heap, O(n log k). A full sort of 9.7M items took 654 ms.
 *
 * Higher score wins; on equal scores the lower index (earlier in the dump)
 * wins, which matches PHP's stable arsort.
 */
export class TopK {
  private readonly k: number
  private readonly scores: Float64Array
  private readonly indices: Int32Array
  private size = 0

  constructor(k: number) {
    this.k = Math.max(0, k)
    this.scores = new Float64Array(this.k)
    this.indices = new Int32Array(this.k)
  }

  /** a is worse than b. */
  private worse(a: number, b: number): boolean {
    const sa = this.scores[a]!
    const sb = this.scores[b]!
    return sa < sb || (sa === sb && this.indices[a]! > this.indices[b]!)
  }

  private swap(a: number, b: number): void {
    const s = this.scores[a]!
    this.scores[a] = this.scores[b]!
    this.scores[b] = s
    const i = this.indices[a]!
    this.indices[a] = this.indices[b]!
    this.indices[b] = i
  }

  push(score: number, index: number): void {
    if (this.k === 0) return
    if (this.size < this.k) {
      let at = this.size++
      this.scores[at] = score
      this.indices[at] = index
      while (at > 0) {
        const parent = (at - 1) >> 1
        if (!this.worse(at, parent)) break
        this.swap(at, parent)
        at = parent
      }
      return
    }
    const worstScore = this.scores[0]!
    if (score < worstScore || (score === worstScore && index > this.indices[0]!)) {
      return
    }
    this.scores[0] = score
    this.indices[0] = index
    let at = 0
    for (;;) {
      const left = at * 2 + 1
      const right = left + 1
      let smallest = at
      if (left < this.size && this.worse(left, smallest)) smallest = left
      if (right < this.size && this.worse(right, smallest)) smallest = right
      if (smallest === at) break
      this.swap(at, smallest)
      at = smallest
    }
  }

  /** Best first. */
  result(): Array<{ score: number; index: number }> {
    const out: Array<{ score: number; index: number }> = []
    for (let i = 0; i < this.size; i++) {
      out.push({ score: this.scores[i]!, index: this.indices[i]! })
    }
    return out.sort((a, b) => b.score - a.score || a.index - b.index)
  }
}
