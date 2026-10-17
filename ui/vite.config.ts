import { svelte } from '@sveltejs/vite-plugin-svelte'
import type { Plugin } from 'vite'
import { defineConfig } from 'vitest/config'

/**
 * Content Security Policy of the built app. A dump exposes array keys, class
 * and property names and frame labels; nothing may leave the page. Styles keep
 * 'unsafe-inline' because Svelte clones static `style=` attributes from
 * template HTML; scripts and the worker are same-origin files.
 */
export const CONTENT_SECURITY_POLICY = [
  "default-src 'self'",
  "script-src 'self'",
  "style-src 'self' 'unsafe-inline'",
  "worker-src 'self'",
  "connect-src 'self'",
  "img-src 'self' data:",
  "font-src 'self'",
  "object-src 'none'",
  "base-uri 'none'",
  "form-action 'none'",
].join('; ')

/** Replaces the `<!-- csp -->` placeholder in index.html, in builds only. */
function contentSecurityPolicy(): Plugin {
  return {
    name: 'meminfo-csp',
    apply: 'build',
    transformIndexHtml: {
      order: 'pre',
      handler(html) {
        if (!html.includes('<!-- csp -->')) {
          throw new Error('index.html is missing the <!-- csp --> placeholder')
        }
        return html.replace(
          '<!-- csp -->',
          `<meta http-equiv="Content-Security-Policy" content="${CONTENT_SECURITY_POLICY}" />`,
        )
      },
    },
  }
}

// https://vite.dev/config/
export default defineConfig({
  // Relative asset URLs: the built app works from any sub-path of a static host.
  base: './',
  plugins: [svelte(), contentSecurityPolicy()],
  test: {
    include: ['src/**/*.test.ts'],
    environment: 'node',
  },
})
