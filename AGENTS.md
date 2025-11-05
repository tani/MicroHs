# Repository Guidelines

## Project Structure & Module Organization
MicroHs is split across `src/MicroHs` (compiler pipeline), `src/runtime` and `lib` (runtime support and bundled packages), and `boards/` plus `cpphssrc/` for device-specific samples. Tooling and helper scripts live under `Tools/`, generated artifacts land in `bin/` and `generated/`, and integration specs and papers sit in `doc/` (see `doc/hs2024.pdf`). Tests and reference outputs live side-by-side in `tests/`, while standalone samples such as `Example.hs` illustrate expected module layout (`module Example(main) where`).

## Build, Test, and Development Commands
- `make bin/gmhs` builds the compiler with GHC, honoring overrides in `ghc/`.
- `make bin/mhs` bootstraps via the combinator/runtime path; use when targeting constrained systems.
- `make minstall` installs into `~/.mcabal`; `make PREFIX=/opt/mhs oldinstall MHSDIR=/opt/mhs` mirrors the legacy flow.
- `cabal build MicroHs && cabal install` uses `MicroHs.cabal` for the host-only toolchain.
- `bin/mhs Example -oEx && ./Ex` is the quickest smoke test for frontend changes.
- `make -C tests alltest MHSTARGET="-O2"` runs the golden suite with optional flag overrides; `make -C tests errtest` exercises compiler diagnostics.

## Coding Style & Naming Conventions
Stay consistent with the repository’s two-space indentation, guard-aligned `=`/`|`, and `where` blocks indented under their binding. Modules follow `MicroHs.X` CamelCase names, exported identifiers are CamelCase, and local helpers are lowerCamelCase; C helpers and board assets retain snake_case. Keep LANGUAGE pragmas minimal (CPP is opt-in) and mirror existing import grouping: Prelude shim first, then qualified modules, then third-party/IO modules.

## Testing Guidelines
Before sending a change, rebuild `bin/gmhs` and run `make -C tests test` for the fast subset; `alltest` additionally covers FFI and concurrency scenarios, while `testapplysp`/`testforexp` verify embedded C toolchains. Each `.hs` test has a `.ref` file—update both only when behaviour intentionally changes and document why. When adjusting runtime C, add a focused program under `tests/` or `boards/` plus its `.ref` output.

## Commit & Pull Request Guidelines
History favors short, imperative titles (`concatenate -optl flags...`, `Regen`) and single-purpose commits; follow that pattern and note regeneration commits explicitly. For pull requests, include: 1) a concise problem statement, 2) build/test evidence (`make bin/gmhs`, `make -C tests alltest`), 3) platform notes (e.g., STM32 board name) if relevant, and 4) any follow-up tasks. Link issues when available and add screenshots/output snippets only when UI or binary size changes warrant it.

## Security & Configuration Tips
Never hardcode device paths or credentials; board samples read configuration from `targets.conf`. Set `MHSDIR` when running binaries outside the tree so runtime assets resolve correctly, and clear `.mhscache` via `make -C tests cache` before sharing cache-dependent results.
