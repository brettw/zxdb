# Standalone zxdb

This repository extracts the Fuchsia debugger into a standalone project.

## Imported Fuchsia libraries

- Place support libraries imported from Fuchsia under `src/lib/`.
- Keep each library in its own named subdirectory, such as `src/lib/fxl/`,
  `src/lib/fit/`, or `src/lib/unwinder/`, regardless of its original location
  in the Fuchsia tree.
- Preserve library boundaries. Do not combine unrelated libraries into a
  shared directory or copy their implementations into debugger subsystems.
- Import only the files and dependencies needed by the standalone debugger.
- Preserve upstream copyright notices and license files. Record the upstream
  source path and revision for each imported library in its directory.
- Keep imported libraries buildable from this repository without requiring a
  neighboring Fuchsia checkout.

## Subsystem guidance

Read applicable nested `AGENTS.md` files before changing subsystem code.
