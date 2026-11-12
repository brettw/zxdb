# Upstream source

Source: Fuchsia `src/lib/uuid`

Revision: `d6be97150582a56d5b731e686ad507dd17686b00`

Only files needed by the standalone host build are imported. See `LICENSE`.

## Standalone changes

Added an explicit `<string.h>` include in `uuid.h`.
