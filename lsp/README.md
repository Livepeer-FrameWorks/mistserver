# MistServer LSP

Web management interface for MistServer. This app is served by MistServer and
embedded into the server binary as static assets.

## Quick Start

```bash
pnpm install
pnpm dev
```

- `pnpm dev` runs esbuild in watch mode and writes `bundle.js`.
- Open `server.html` against a running MistServer instance.

## Build For Production

```bash
pnpm build
```

This runs `generateLSP.sh`, which:

1. Runs preflight checks (unless `LSP_SKIP_PREFLIGHT=1`)
2. Assembles `main.css` from `css/*.css`
3. Bundles/minifies `modules/main.js` to `minified.js`

`minified.js` and `main.css` are checked in and consumed by Meson (`meson.build`)
for embedding into the MistServer binary.

## Daily Workflow

```bash
pnpm dev        # fast feedback loop
pnpm test       # smoke + manifest helper tests
pnpm lint       # syntax check
pnpm preflight  # full gate: manifest + syntax + css-lint + smoke + unit
pnpm build      # production artifact refresh
```

## Project Map

```text
modules/
  core/         app shell, state, tabs, sockets, forms, API plumbing
  pages/        top-level page handlers (overview, general, protocols, ...)
  streams/      stream list/detail/config and helpers
  pushes/       push list/config and helpers
  cameras/      camera inventory, detail, ptz, dialogs
  components/   reusable UI building blocks
  fun/          non-critical extras
  main.js       single entrypoint and load order authority

css/            source stylesheets (concatenated into main.css)
plugins/        vendored non-npm dependencies
scripts/        preflight, smoke, syntax, and helper checks
test/           node-based unit tests
```
