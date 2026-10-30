# php-meminfo

## Overview

PHP extension and analysis tools for investigating memory usage and leaks. The extension dumps a graph of PHP allocations as JSON; the browser UI analyzes dumps locally, and the CLI requests dumps from running processes.

## Stack and capabilities

- C extension using the PHP/Zend API, built with phpize and Make; Composer metadata targets PHP 7.0 and later.
- Optional S3 upload using aws-c-s3 and vcpkg.
- PHP command-line trigger in `cli/`.
- Svelte 5 and TypeScript browser UI, built with Vite; parsing and graph analysis run in a worker.
- PHPT extension tests, Vitest unit tests and Playwright browser tests; GitHub Actions runs CI.
- No application database or server framework is required.

## Architecture

The extension, CLI and UI communicate through process signals and the JSON dump format. See `.ai-factory/ARCHITECTURE.md` for boundaries and development guidance.
