import { defineConfig } from 'vite'

/**
 * Builds report/cli.ts, the headless report, into dist-report/cli.js for Node:
 * the engine modules import each other without file extensions, which Node
 * does not resolve on its own.
 */
export default defineConfig({
  build: {
    ssr: 'report/cli.ts',
    outDir: 'dist-report',
    emptyOutDir: true,
    target: 'node24',
    rollupOptions: {
      output: { entryFileNames: 'cli.js' },
    },
  },
})
