import { describe, expect, it } from 'vitest'
import { histogramTrend, InvalidHistogramError, parseHistogram } from './histogram'

describe('histograms', () => {
  it('reads the bare meminfo_histogram() array and the wrapped form', () => {
    const bare = parseHistogram('{"App\\\\Entry":{"count":3,"size":216}}', 'a.json', 5_000)
    expect(bare.time).toBe(5)
    expect(bare.classes.get('App\\Entry')).toEqual({ count: 3, size: 216 })

    const wrapped = parseHistogram('{"timestamp":100,"classes":{"Entry":{"count":1,"size":72}}}', 'b.json', 0)
    expect(wrapped.time).toBe(100)
  })

  it('rejects what is not a histogram', () => {
    expect(() => parseHistogram('[1,2]', 'x.json', 0)).toThrow(InvalidHistogramError)
    expect(() => parseHistogram('{"header":{},"items":{}}', 'dump.json', 0)).toThrow(/count/)
  })

  it('orders samples by time and puts classes growing at every sample first', () => {
    const points = [
      parseHistogram('{"timestamp":3,"classes":{"Leak":{"count":30,"size":0},"Noise":{"count":5,"size":0}}}', 'c', 0),
      parseHistogram('{"timestamp":1,"classes":{"Leak":{"count":10,"size":0},"Noise":{"count":9,"size":0}}}', 'a', 0),
      parseHistogram('{"timestamp":2,"classes":{"Leak":{"count":20,"size":0},"Noise":{"count":1,"size":0}}}', 'b', 0),
    ]
    const trend = histogramTrend(points)
    expect(trend.names).toEqual(['a', 'b', 'c'])
    expect(trend.rows.map((row) => [row.key, row.counts, row.growing])).toEqual([
      ['Leak', [10, 20, 30], true],
      ['Noise', [9, 1, 5], false],
    ])
  })
})
