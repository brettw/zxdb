# Upstream source

Source: Fuchsia `src/lib/analytics`

Revision: `d6be97150582a56d5b731e686ad507dd17686b00`

Only files needed by the standalone host build are imported. See `LICENSE`.

## Standalone changes

Changed the source-root syslog include to `<lib/syslog/cpp/macros.h>`. Used pointer-and-length RapidJSON `Key()` calls for compatibility with the system RapidJSON 1.1 headers.
