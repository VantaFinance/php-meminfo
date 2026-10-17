/**
 * Small levelled logger shared by the page and the worker.
 *
 * Off (WARN) by default and in production. Enable verbose output with
 * `localStorage.meminfoDebug = '1'` or `?debug=1`; the page passes its level to
 * the worker, which has neither. Never log item contents: a dump holds the
 * application's keys, class names and frame labels.
 */

export type LogLevel = 'debug' | 'info' | 'warn' | 'error'

const ORDER: Record<LogLevel, number> = { debug: 10, info: 20, warn: 30, error: 40 }

let current: LogLevel = 'warn'

export function setLogLevel(level: LogLevel): void {
  current = level
}

export function getLogLevel(): LogLevel {
  return current
}

/** Level requested by the page: `?debug=1` or `localStorage.meminfoDebug`. */
export function detectPageLogLevel(): LogLevel {
  try {
    const query = new URLSearchParams(globalThis.location?.search ?? '')
    if (query.get('debug') === '1') {
      return 'debug'
    }
    const stored = globalThis.localStorage?.getItem('meminfoDebug')
    if (stored === '1' || stored === 'debug') {
      return 'debug'
    }
    if (stored === 'info') {
      return 'info'
    }
  } catch {
    // Storage can be unavailable (private mode, sandboxed frames).
  }
  return 'warn'
}

export interface Logger {
  debug(message: string, data?: Record<string, unknown>): void
  info(message: string, data?: Record<string, unknown>): void
  warn(message: string, data?: Record<string, unknown>): void
  error(message: string, data?: Record<string, unknown>): void
}

export function createLogger(area: string): Logger {
  const prefix = `[meminfo-ui:${area}]`

  const emit = (level: LogLevel, message: string, data?: Record<string, unknown>): void => {
    if (ORDER[level] < ORDER[current]) {
      return
    }
    const sink = level === 'debug' ? console.debug : level === 'info' ? console.info : level === 'warn' ? console.warn : console.error
    if (data === undefined) {
      sink(prefix, message)
    } else {
      sink(prefix, message, data)
    }
  }

  return {
    debug: (message, data) => emit('debug', message, data),
    info: (message, data) => emit('info', message, data),
    warn: (message, data) => emit('warn', message, data),
    error: (message, data) => emit('error', message, data),
  }
}
