# Upstream source

Source: Fuchsia `src/lib/unwinder`

Revision: `d6be97150582a56d5b731e686ad507dd17686b00`

Only files needed by the standalone host build are imported. See `LICENSE`.

## Standalone changes

Changed source-root fit includes to `<lib/fit/function.h>` in `memory.h`, `unwind.h`, and `unwinder_base.h`. Replaced range formatting in `Registers::Describe()` with an equivalent join because libstdc++ 14 does not provide the required range formatter.
