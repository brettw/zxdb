# Upstream source

Source: Fuchsia `src/lib/files`

Revision: `d6be97150582a56d5b731e686ad507dd17686b00`

Only files needed by the standalone host build are imported. See `LICENSE`.

## Standalone changes

Added explicit `<string.h>` includes in `glob.cc` and `scoped_temp_dir.cc` for the host standard library.
