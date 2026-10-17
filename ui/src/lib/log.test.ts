import { afterEach, describe, expect, it, vi } from 'vitest'
import { createLogger, setLogLevel } from './log'

describe('logger', () => {
  afterEach(() => {
    setLogLevel('warn')
    vi.restoreAllMocks()
  })

  it('drops messages below the current level', () => {
    const debug = vi.spyOn(console, 'debug').mockImplementation(() => {})
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    const log = createLogger('test')

    log.debug('hidden')
    log.warn('shown', { n: 1 })

    expect(debug).not.toHaveBeenCalled()
    expect(warn).toHaveBeenCalledWith('[meminfo-ui:test]', 'shown', { n: 1 })
  })

  it('emits debug output once the level is lowered', () => {
    const debug = vi.spyOn(console, 'debug').mockImplementation(() => {})
    setLogLevel('debug')

    createLogger('test').debug('visible')

    expect(debug).toHaveBeenCalledWith('[meminfo-ui:test]', 'visible')
  })
})
