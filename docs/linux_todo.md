# Linux debugger remaining work

No outstanding fixes remain from this audit. Completed findings and findings
that no longer apply have been removed.

Intentional platform limits are documented in
[Linux debugging behavior](../README.md#linux-debugging-behavior).

Regression coverage includes per-thread attach, fork/clone/vfork, exec, final
reaping, signal forwarding, group stops, memory and register access, breakpoint
restoration, exact instruction stepping, output draining, and safe shutdown.
Run `ctest --test-dir build --output-on-failure` after building.
