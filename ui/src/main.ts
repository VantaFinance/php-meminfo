import { mount } from 'svelte'
import './app.css'
import App from './App.svelte'
import { createLogger, detectPageLogLevel, setLogLevel } from './lib/log'

setLogLevel(detectPageLogLevel())

const log = createLogger('app')

// First thing: anything the CSP blocks is something that tried to leave the page.
document.addEventListener('securitypolicyviolation', (event) => {
  log.warn('blocked by the content security policy', {
    directive: event.effectiveDirective,
    blocked: event.blockedURI,
  })
})

log.info('starting', { mode: import.meta.env.MODE, level: detectPageLogLevel() })

const target = document.getElementById('app')
if (target === null) {
  throw new Error('#app element is missing from index.html')
}

const app = mount(App, { target })

export default app
