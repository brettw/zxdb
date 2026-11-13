# Linux feature parity TODO

> Remove items from this file as they are completed. Do not keep checked-off
> items as history. Before removing an implementation item, verify its completion
> criteria and add appropriate regression coverage. For a platform-specific item,
> an explicit, documented decision that no Linux equivalent is needed can close
> the item; update the relevant help and capability reporting as necessary.

## Scope and evidence

Audit baseline: commit `013aa93`, 2026-11-13. This compares the Fuchsia backend
and frontend features **present in this repository** with the standalone native
Linux agent. It does not compare against a newer upstream Fuchsia checkout.
The current Linux build supports x86-64 only.

The audit covers the platform interfaces, all 26 IPC request types, notification
producers, register implementations, console and DAP consumers, expression/TLS
helpers, and build/agent startup paths. Findings are based on source inspection
unless marked **reproduced**. No live Fuchsia comparison was run. Proposed tests
below are completion criteria, not tests that passed during this audit.

Priorities: **P1** = crashes, incorrect behavior, or core debugging gaps;
**P2** = additional workflows or architecture support; **P3** = diagnostic
detail. Items under “Platform-specific decisions” need a Linux design or an
explicit scope decision, rather than a direct translation of Zircon APIs.
IDs are stable references; do not renumber the remaining items after removal.

## Portable features and defects

### Process discovery and attachment

- [ ] **L01 · P1 · Fix attachment by process name (reproduced agent crash).**
  Fuchsia initializes `DebugAgent::root_job_`; Linux does not.
  `OnUpdateFilter()` nevertheless asserts and dereferences it for every nonempty
  filter update. A local `attach zxdb_audit_no_such_process` prints that it is
  waiting, then aborts the agent with `Check failed: root_job_` at
  `debug_agent.cc:564`. This affects existing-process matching too, including DAP
  pattern attachment. The root README's claim that existing-process filters work
  is therefore incorrect at this baseline. Linux can enumerate `/proc`, but that
  does not make the filter request path work.
  **Complete when:** existing-name attach, no-match, multiple-match, filter
  removal, and process-exit races work without an assertion or null dereference;
  update the README. Test console and DAP paths.
  Sources: [agent initialization and OnUpdateFilter][agent],
  [Linux job enumeration and unsupported Attach][linux-job].

- [ ] **L02 · P1 · Give process filters Linux naming rules.**
  Shared `ResolveFilterType()` truncates process patterns to 31 characters and
  interprets `/path` as a component moniker, a slash-containing name as a moniker
  suffix, and `.cm` as a component name. These Fuchsia rules are used for Linux
  console/DAP filters too. Linux process names come from `argv[0]` or `/proc/exe`,
  so fixing L01 alone does not provide reliable path or long-name matching.
  **Complete when:** Linux exact/substring matching has documented executable
  path and basename semantics; long names, slashes, `.cm` names, and DAP patterns
  work without changing Fuchsia resolution. Depends on L01 for end-to-end tests.
  Sources: [ResolveFilterType][filters], [LinuxProcessHandle::GetName][linux-process].

- [ ] **L03 · P1 · Observe future unrelated processes and name changes.**
  Fuchsia job watches report process creation and process-name changes, enabling
  filters to attach before the process proceeds. Linux job attachment returns
  `No jobs on Linux`; its ptrace events cover traced tasks and their descendants,
  not arbitrary future processes. The console's promise that filters attach to
  newly launched processes is misleading for native Linux.
  **Complete when:** a supported Linux monitoring strategy implements and tests
  future-name matching, rename/exec matching, removal, duplicate suppression, and
  permission failures. Define whether stopping before user code is guaranteed;
  document any weaker timing guarantee. Following already-traced forks is not
  sufficient. Depends on L01–L02; also affects terminal-based DAP launch (L12).
  Sources: [Zircon job events][zircon-job], [Linux job handle][linux-job],
  [Linux task events][linux-task].

### Registers, breakpoints, and expression evaluation

All four register categories have frontend/protocol support. Linux currently
returns values only for `kGeneral`; unsupported categories return an empty list.
Its write implementation accepts only the general-register whitelist.

| Category | Fuchsia x86-64 implementation | Native Linux gap |
| --- | --- | --- |
| General | Integer registers, RIP, RFLAGS, FSBASE/GSBASE reads | Linux already reads and writes these 20 registers; no blanket general-register TODO |
| Floating point | FCW, FSW, FTW, FOP, FIP, FDP, ST0–ST7 | All missing (L07) |
| Vector | MXCSR and ZMM0–ZMM31 representation, with frontend XMM/YMM aliases | All missing (L08); actual availability must follow CPU/kernel support |
| Debug | DR0–DR3, DR6, DR7 | Read/write and backend access missing (L04) |

Sources: [Linux thread registers][linux-thread], [Linux general-register mapping][linux-x64],
[Fuchsia x86-64 register reads/writes][zircon-x64].

- [ ] **L04 · P1 · Implement debug-register access.**
  Linux `GetDebugRegisters()` returns `nullopt`, `SetDebugRegisters()` returns
  false, and register requests do not expose the debug category. Fuchsia supports
  both backend access and user-visible reads/writes. This blocks `regs --debug`
  and the register substrate needed by L05–L06.
  **Complete when:** DR0–DR3/DR6/DR7 can be read and valid changes written on a
  stopped thread; invalid writes fail clearly; thread-local state and debugger
  breakpoint ownership remain consistent. Test reads, writes, and cleanup.
  Sources: [Linux thread handle][linux-thread], [Zircon thread handle][zircon-thread],
  [Fuchsia register conversion][zircon-x64].

- [ ] **L05 · P1 · Implement hardware execution breakpoints.**
  Linux installation/removal return false and the advertised hardware breakpoint
  count is zero. Fuchsia installs execution addresses in hardware debug slots.
  **Complete when:** hardware execution breakpoints hit on the intended threads,
  step over correctly, survive supported thread/process lifecycle changes, and
  are removed on deletion/detach. Test new threads, slot exhaustion, coexistence
  with watchpoints, and exec. Report usable capacity only after implementation.
  Depends on L04. Sources: [Linux thread handle][linux-thread],
  [Linux slot counts][linux-x64], [Zircon thread handle][zircon-thread].

- [ ] **L06 · P1 · Implement hardware data watchpoints.**
  Linux `InstallWatchpoint()`/`UninstallWatchpoint()` are stubs and the reported
  watchpoint count is zero. Fuchsia supports write and read/write watchpoints.
  Linux also maps every `TRAP_HWBKPT` to an execution breakpoint; a complete port
  must identify the triggering slot and distinguish data hits. Console watches,
  data-triggered tracepoints, and DAP data breakpoints depend on this support.
  **Complete when:** supported sizes, alignments, access modes, and thread scopes
  work; invalid ranges and exhausted shared slots report errors; hit attribution,
  stepping, new threads, exec, deletion, and detach are tested. Depends on L04.
  Sources: [Linux watchpoint stubs][linux-thread], [trap decoding][linux-x64],
  [Fuchsia installation][zircon-thread].

- [ ] **L07 · P1 · Read and write x87 floating-point state.**
  Implement the floating-point category shown above, including the control/status
  fields and stack-register format expected by the frontend. Fuchsia already
  supplies this state; Linux returns none.
  **Complete when:** known x87 values and control fields can be inspected and
  changed, widths/padding are handled correctly, and unrelated register state is
  preserved. Test round trips on multiple stopped threads.
  Sources: [Linux ReadRegisters/WriteRegisters][linux-thread],
  [Fuchsia floating-point conversion][zircon-x64].

- [ ] **L08 · P1 · Read and write SIMD/vector state.**
  Add MXCSR and the vector state represented by the existing XMM/YMM/ZMM register
  model. Linux currently reads only the general register set. This also prevents
  evaluation of variables and supported return values that live in SIMD registers.
  **Complete when:** register queries, expression reads, and writes work for the
  CPU/kernel-supported state; unsupported extensions report unavailability rather
  than fabricated zero values. Test partial-register writes, preservation of
  unrelated state, MXCSR, and SSE/AVX/AVX-512 where available.
  Sources: [Linux register implementation][linux-thread],
  [Fuchsia vector conversion][zircon-x64], [x86-64 return-value ABI][abi-x64].

- [ ] **L09 · P1 · Validate expression calls and return values with full state.**
  This is a dependent gap, not a claim that all Linux function calls are absent.
  The shared x86-64 call controller saves general, floating-point, and vector
  categories, then restores the saved values. Empty Linux FP/vector responses
  leave that state unsaved. Calls can therefore change state that the debugger
  cannot restore; supported floating-point return decoding also needs L08.
  **Complete when:** after L07–L08, real-inferior tests verify register preservation
  across injected calls, error/cancel paths, and supported integer/floating-point
  return-value displays. Until then, reject operations that need unavailable
  state instead of presenting them as safe. Shared ABI limitations are not Linux
  parity work.
  Sources: [x86-64 call controller][call-x64],
  [call cleanup][call-base], [return-value ABI][abi-x64].

- [ ] **L10 · P1 · Resolve native Linux thread-local variables.**
  `ProcessImpl::LoadTLSHelpers()` requires the Fuchsia debugger integration
  symbols `zxdb.thrd_t`, `zxdb.link_map_tls_modid`, and `zxdb.tlsbase`. There is no
  native Linux fallback. Binaries without these hooks fail with
  `This binary is missing debugger integration hooks for reading TLS.`
  Reading FSBASE alone does not provide the missing per-module TLS resolution.
  **Complete when:** thread-local expressions work for the explicitly supported
  Linux libc/runtime combinations. Test distinct values in multiple threads,
  executable and shared-library TLS, dynamically loaded modules, and cache reset
  on exec. Report unsupported TLS layouts accurately.
  Sources: [helper loading][process], [TLS address evaluation][process-symbols].

### Dumps, IDE workflows, deployment, and architecture

- [ ] **L11 · P2 · Save a minidump from a live Linux process.**
  Linux `SaveMinidump()` returns `SaveMinidump is unimplemented on Linux.`
  Fuchsia snapshots stopped threads and writes a Crashpad minidump. Existing
  minidump opening/inspection is a separate, implemented frontend path.
  **Complete when:** `save-dump` produces a readable dump with the supported
  threads, registers, modules, memory, and exception information; failure leaves
  the inferior in the correct state. Reopen the dump and verify stacks/values.
  Sources: [Linux SaveMinidump][linux-process],
  [Fuchsia SaveMinidump][zircon-process], [minidump reader][minidump].

- [ ] **L12 · P1 · Implement a native Linux DAP launch workflow.**
  Without `RunInTerminal`, the launch handler accepts only a component URL and
  calls `RunComponent`. With `RunInTerminal`, it installs an attach filter and
  asks the client to launch a command. It does not route native ELF launch to
  `RunBinary`; the filter route is blocked by L01–L03. Numeric-PID DAP attachment
  and console `run` are separate implemented paths.
  **Complete when:** a documented Linux launch request launches the intended
  executable/arguments, honors the advertised options, binds breakpoints before
  the intended stop, and reports launch failure to the IDE. Test both supported
  client modes; implement or explicitly reject terminal launch until reliable.
  Sources: [DAP launch][dap-launch], [DAP attach][dap-attach],
  [console binary launch][console-run].

- [ ] **L13 · P1 · Make capabilities and unsupported-feature errors accurate.**
  DAP initialization advertises `supportsDataBreakpoints = true` even though the
  Linux backend has no watchpoints; data-breakpoint discovery exposes write and
  read/write modes. Register reads silently omit unsupported categories. Shared
  filter/help text also promises facilities Linux does not provide.
  **Complete when:** DAP, console help/status, and backend errors agree with the
  connected target's capabilities, including before/after connection. Cover
  watchpoints, register categories, process filters, and Fuchsia-only operations.
  Do not disable working shared DAP features wholesale. Revisit as other items
  are implemented.
  Sources: [DAP initialization][dap-context], [DAP data breakpoints][dap-data],
  [Linux register responses][linux-thread], [filter request path][agent].

- [ ] **L14 · P2 · Provide a deployable remote Linux agent/session workflow.**
  Fuchsia has a service/launcher and debug-agent connection setup. The standalone
  CMake build makes `debug_agent` a static library and starts the Linux agent as
  a child connected by anonymous pipes. It provides no standalone Linux agent
  listener executable. `Session::Disconnect()` rejects disconnecting this
  persistent local connection. The DAP listener connects an IDE to the frontend;
  it is not a remote target-agent endpoint.
  **Complete when:** an agent can run on a separate Linux machine and a frontend
  can connect through a documented transport, with tested disconnect/cleanup and
  subsequent connection behavior. Define whether reconnect preserves a session.
  Existing frontend remote-connect code need not be replaced.
  Sources: [agent CMake target][agent-cmake], [local agent startup][local-agent],
  [session lifecycle][session], [Fuchsia agent entry point][agent-main].

- [ ] **L15 · P2 · Support native Linux AArch64.**
  Fuchsia has an ARM64 backend. Standalone CMake rejects non-x86-64 hosts, and
  `linux_arch_arm64.cc` contains `FX_NOTREACHED()` stubs. Linux register handling
  and task syscall decoding also contain x86-64 assumptions; selecting a
  different architecture source file is insufficient.
  **Complete when:** build, launch/attach, registers, software/hardware
  breakpoints, watchpoints, stepping, signal handling, unwinding, and supported
  fork/exec lifecycle tests pass on ARM64. Report architecture-specific capacity
  and register availability. This does not imply a RISC-V or 32-bit parity gap;
  those Fuchsia implementations are not established by this tree.
  Sources: [top-level build][cmake], [Linux ARM64 stubs][linux-arm64],
  [Linux thread handle][linux-thread], [Linux task decoding][linux-task],
  [Fuchsia ARM64 backend][zircon-arm64].

- [ ] **L16 · P3 · Report the Linux system version.**
  Linux `GetSystemVersion()` always returns `Unknown`; Fuchsia supplies its
  system version. CPU count and physical memory are already implemented on Linux.
  **Complete when:** `SysInfo` and its console consumer show a meaningful Linux
  version with a defined failure fallback and test coverage.
  Sources: [Linux system interface][linux-system],
  [Fuchsia system interface][zircon-system].

## Platform-specific decisions

- [ ] **L17 · P2 · Define observation-only attachment.**
  Fuchsia can watch process lifetime without claiming its exception channel.
  Linux explicitly rejects `claim_exception_channel = false`, so minimal-priority
  process tracking is unavailable. Weak and strong attachment both use full
  ptrace attachment; weak attachment is not an untraced observer.
  **Close when:** implement/test a useful Linux observation mode, or document
  minimal-priority attachment as unavailable and reject it consistently.
  Sources: [Linux attachment][linux-process], [Fuchsia attachment][zircon-process],
  [priority-to-attachment mapping][debugged-process].

- [ ] **L18 · P2 · Define second-chance exception behavior.**
  Fuchsia reads/writes the exception strategy through Zircon. Linux always
  reports first chance; setting second chance merely forwards the signal on
  resume. This is not a later debugger stop after application handling. The
  shared `second-chance-exceptions` setting needs platform-aware semantics.
  Linux signal number/code/address/sender reporting and `continue --forward`
  are already implemented.
  **Close when:** provide and test a clearly defined Linux policy for handled
  and fatal signals, or reject the Fuchsia strategy setting on Linux while
  retaining explicit forwarding. Do not promise Zircon exception-channel order.
  Sources: [Linux exceptions][linux-exception],
  [Fuchsia exceptions][zircon-exception], [shared settings][system].

- [ ] **L19 · P2 · Define process grouping and job-scoped filters.**
  Fuchsia exposes a job tree, job IDs, job attachment, and recursive job/component
  filters. Linux exposes a synthetic root with a flat `/proc` process list,
  no child jobs, and no process job ID. Linux parent PIDs, process groups, and
  cgroups are not interchangeable with Zircon jobs.
  **Close when:** choose any supported Linux grouping model and test its scope
  semantics, or consistently mark job-only/recursive component operations as
  Fuchsia-only. Keep ordinary process enumeration and L01–L03 independent.
  Sources: [Linux job interface][linux-job-h], [Linux job implementation][linux-job],
  [Linux process interface][linux-process-h], [Fuchsia jobs][zircon-job].

- [ ] **L20 · P2 · Decide whether to provide crash holding / deferred attachment.**
  Fuchsia's limbo service holds crashed processes for later inspection and lets
  the debugger retrieve/release their exceptions. `LinuxLimboProvider` is always
  invalid, has no records, and returns errors for retrieval/release. Thus the
  waiting-on-exception and deferred-crash-attach workflow has no Linux provider.
  **Close when:** implement a defined Linux crash-capture workflow with lifecycle
  tests, or document limbo as Fuchsia-only. Attaching to a process already stopped
  by this debugger is not equivalent to catching an untraced crash for later use.
  Sources: [Linux limbo stub][linux-limbo], [Fuchsia limbo provider][zircon-limbo].

- [ ] **L21 · P3 · Define a Linux equivalent for handle inspection.**
  Fuchsia exposes handle types, rights, related objects, and VMO information.
  Linux `GetHandles()` returns `No handles on Linux`; there is no file-descriptor
  inspection implementation behind that request.
  **Close when:** implement a documented Linux descriptor/resource view with
  Linux-appropriate fields and permission/race tests, or mark Zircon handle
  inspection as Fuchsia-only. Do not label file descriptor numbers as KOIDs or
  invent Zircon rights for them.
  Sources: [Linux handle request][linux-process], [Fuchsia handle table][zircon-process].

- [ ] **L22 · P3 · Decide which memory-accounting details to expose.**
  Linux address-space inspection already supplies mapping names, ranges,
  permissions, offsets, and shared/private flags. Fuchsia additionally supplies
  nested VMAR depth, VMO identity, and committed-page accounting. Those fields
  are absent on Linux and the console deliberately hides Fuchsia-only columns.
  **Close when:** implement useful Linux accounting with clearly named metrics
  and tests, or document the difference as intentional. Linux resident/proportional
  memory metrics must not be silently presented as Zircon committed pages.
  Sources: [Linux mappings][linux-process], [Fuchsia mappings][zircon-process],
  [address-space formatting][console-aspace].

- [ ] **L23 · P2 · Define support for processes sharing an address space.**
  Fuchsia reports shared address-space regions used by restricted/shared-process
  execution; the client can adapt breakpoint placement for them. Linux
  `GetSharedAddressSpace()` returns `nullopt`, and non-thread `CLONE_VM` is
  deliberately left stopped with an unsupported-operation error. Ordinary
  shared mappings and threads are already handled and are different cases.
  **Close when:** either implement a Linux ownership/lifecycle model for these
  processes, including breakpoint restoration and detach/exit tests, or retain
  an explicit documented limitation. Fuchsia shared regions and `CLONE_VM`
  require separate semantics; they are not exact API equivalents.
  Sources: [Linux shared-space/clone handling][linux-process],
  [Fuchsia shared-space discovery][zircon-process], [client breakpoints][breakpoint].

- [ ] **L24 · P3 · Define richer blocked-thread diagnostics.**
  Fuchsia distinguishes blocked reasons such as futex, channel, port, and pager
  waits. Linux maps `/proc` task states to broad states such as sleeping,
  suspended, and exception, without comparable wait-reason detail.
  **Close when:** expose useful Linux-specific reasons where observable and test
  representative waits, or document the coarser state model. Do not infer a
  specific wait primitive solely from a generic sleeping state.
  Sources: [Linux state mapping][linux-utils], [Fuchsia thread state][zircon-thread].

- [ ] **L25 · P2 · Keep component/test services explicitly platform-scoped.**
  Fuchsia supports component URL/moniker discovery, recursive component filters,
  component launch, test-realm/case-filter launch, and their lifecycle/log events.
  Linux component lookup is empty; `LaunchComponent`/`LaunchTest` return
  `No components on Linux`, and non-ELF component lookup is unimplemented.
  **Close when:** document and gate these as Fuchsia services, or define a
  separate Linux service/test-runner integration if needed. Native binary launch
  is already present; adapting DAP launch is tracked in L12. Do not treat `ffx`,
  component packaging, FIDL services, or Fuchsia log routing as required Linux
  libraries merely to close parity items.
  Sources: [Linux component manager][linux-component],
  [Fuchsia component manager][zircon-component], [IPC events][protocol].

- [ ] **L26 · P3 · Decide which Linux async runtimes to inspect.**
  The shared process model registers `FuchsiaAsyncRustTaskProvider` and
  `FdfDispatcherTaskProvider`. Async-task/backtrace support depends on those
  runtimes' symbols/layouts; it does not supply providers for unrelated native
  Linux executors or event loops. Ordinary synchronous backtraces are shared.
  **Close when:** document the supported runtimes/platform scope, or add selected
  Linux runtime providers using DWARF-based inspection and fixture tests. The
  presence of the generic async-backtrace command alone is not runtime support.
  Sources: [provider registration][process], [Rust provider][async-rust],
  [driver dispatcher provider][async-fdf].

- [ ] **L27 · P3 · Improve fault diagnostics where Linux exposes the data.**
  Fuchsia exception records contain architecture fault details: x86-64 trap
  vector/error code/CR2, or ARM64 ESR/FAR. The console decodes these into details
  such as read/write/instruction-fetch faults. Linux leaves that architecture
  record invalid and instead supplies signal metadata; the console currently
  prints numeric signal/code/address information. This is useful but less
  descriptive, and ordinary register access does not fill the gap.
  **Close when:** decode the available Linux signal reasons and expose additional
  fault details only where they can be obtained reliably, or document the
  remaining diagnostic differences. Test representative memory and arithmetic
  faults and user-generated signals. Do not synthesize unavailable trap-register
  values. This is separate from the second-chance policy in L18.
  Sources: [Linux exception records][linux-exception],
  [Fuchsia x86-64 fault records][zircon-x64],
  [Fuchsia ARM64 fault records][zircon-arm64], [exception formatting][format-exception].

## Coverage and exclusions

This table records the request surface inspected. “Implemented path” means an
implementation exists; it is not a claim that every edge case was tested here.

| IPC requests | Audit result |
| --- | --- |
| `Hello`, `Status` | Implemented; hardware capabilities and limbo differ (L05–L06, L20) |
| `Kill`, `Attach`, `Detach`, `Pause`, `Resume` | Direct PID/task control implemented; minimal attach and exception strategy differ (L17–L18) |
| `ProcessTree`, `UpdateFilter` | Flat process listing exists; filters are defective/incomplete (L01–L03, L19) |
| `Threads`, `ThreadStatus` | Implemented thread/stack paths; wait detail differs (L24) |
| `ReadMemory`, `WriteMemory` | Implemented, including software-breakpoint-aware handling |
| `AddOrChangeBreakpoint`, `RemoveBreakpoint` | Software implementation exists; hardware execution/data gaps (L04–L06) |
| `SysInfo` | Version placeholder and zero hardware slot counts (L05–L06, L16) |
| `AddressSpace`, `Modules` | Implemented mappings and ELF module discovery; extra accounting/shared-space gaps (L22–L23) |
| `LoadInfoHandleTable` | No Linux implementation (L21) |
| `UpdateGlobalSettings` | Shared handler exists; second-chance semantics differ (L18) |
| `SaveMinidump` | Missing Linux writer (L11) |
| `ReadRegisters`, `WriteRegisters` | General only (L04, L07–L09) |
| `RunBinary` | Native Linux launcher exists |
| `RunComponent`, `RunTest` | Fuchsia services (L25) |

Notification paths for traced process/thread start and exit, exceptions, modules,
and captured stdout/stderr exist on Linux. System-wide discovery, component/test
lifecycle, component-derived filter creation, and Fuchsia service log delivery
depend on the missing or platform-specific facilities above. A shared wire type
or frontend handler is not evidence that the Linux backend produces the event.

Do not re-add these as blanket missing features: PID attachment, native binary
launch, software breakpoints, pause/resume, source/instruction stepping, general
register writes, signal forwarding/details, thread lifecycle, traced fork/vfork/
exec handling, mapped ELF/module lookup, ELF/DWARF symbol reading, ordinary
expression evaluation, synchronous unwinding, and minidump reading. Their
implemented paths remain subject to normal bug reports and regression testing.

Native PTY/input support, PATH search, configurable fork-follow policy, and broad
launch environment controls may be useful Linux enhancements, but this audit did
not establish corresponding supported Fuchsia frontend features. Likewise,
segment-selector registers, extra register IDs absent from the shared protocol,
and shared expression/DWARF limitations are not automatically Linux parity gaps.

Verification for this documentation change: source comparison and one local
process-name filter probe. The probe's frontend exit code was zero even though
the child agent printed the fatal assertion; the diagnostic, not the exit code,
establishes L01. No implementation changes or full test-suite run were made.

[agent]: ../src/developer/debug/debug_agent/debug_agent.cc
[linux-job]: ../src/developer/debug/debug_agent/linux_job_handle.cc
[linux-job-h]: ../src/developer/debug/debug_agent/linux_job_handle.h
[zircon-job]: ../src/developer/debug/debug_agent/zircon_job_handle.cc
[filters]: ../src/developer/debug/ipc/filter_utils.cc
[linux-process]: ../src/developer/debug/debug_agent/linux_process_handle.cc
[linux-process-h]: ../src/developer/debug/debug_agent/linux_process_handle.h
[zircon-process]: ../src/developer/debug/debug_agent/zircon_process_handle.cc
[debugged-process]: ../src/developer/debug/debug_agent/debugged_process.cc
[linux-task]: ../src/developer/debug/debug_agent/linux_task.cc
[linux-thread]: ../src/developer/debug/debug_agent/linux_thread_handle.cc
[zircon-thread]: ../src/developer/debug/debug_agent/zircon_thread_handle.cc
[linux-x64]: ../src/developer/debug/debug_agent/linux_arch_x64.cc
[zircon-x64]: ../src/developer/debug/debug_agent/zircon_arch_x64.cc
[abi-x64]: ../src/developer/debug/zxdb/expr/abi_x64.cc
[call-x64]: ../src/developer/debug/zxdb/client/call_function_thread_controller_x64.cc
[call-base]: ../src/developer/debug/zxdb/client/call_function_thread_controller.cc
[process]: ../src/developer/debug/zxdb/client/process_impl.cc
[process-symbols]: ../src/developer/debug/zxdb/client/process_symbol_data_provider.cc
[minidump]: ../src/developer/debug/zxdb/client/minidump_remote_api.cc
[dap-launch]: ../src/developer/debug/zxdb/debug_adapter/handlers/request_launch.cc
[dap-attach]: ../src/developer/debug/zxdb/debug_adapter/handlers/request_attach.cc
[dap-context]: ../src/developer/debug/zxdb/debug_adapter/context.cc
[dap-data]: ../src/developer/debug/zxdb/debug_adapter/handlers/request_data_breakpoint.cc
[console-run]: ../src/developer/debug/zxdb/console/commands/verb_run.cc
[agent-cmake]: ../src/developer/debug/debug_agent/CMakeLists.txt
[local-agent]: ../src/developer/debug/zxdb/local_agent.cc
[session]: ../src/developer/debug/zxdb/client/session.cc
[agent-main]: ../src/developer/debug/debug_agent/main.cc
[cmake]: ../CMakeLists.txt
[linux-arm64]: ../src/developer/debug/debug_agent/linux_arch_arm64.cc
[zircon-arm64]: ../src/developer/debug/debug_agent/zircon_arch_arm64.cc
[linux-system]: ../src/developer/debug/debug_agent/linux_system_interface.h
[zircon-system]: ../src/developer/debug/debug_agent/zircon_system_interface.cc
[linux-exception]: ../src/developer/debug/debug_agent/linux_exception_handle.cc
[zircon-exception]: ../src/developer/debug/debug_agent/zircon_exception_handle.cc
[system]: ../src/developer/debug/zxdb/client/system.cc
[linux-limbo]: ../src/developer/debug/debug_agent/linux_limbo_provider.h
[zircon-limbo]: ../src/developer/debug/debug_agent/zircon_limbo_provider.cc
[console-aspace]: ../src/developer/debug/zxdb/console/commands/verb_aspace.cc
[breakpoint]: ../src/developer/debug/zxdb/client/breakpoint_impl.cc
[linux-utils]: ../src/developer/debug/debug_agent/linux_utils.cc
[linux-component]: ../src/developer/debug/debug_agent/linux_component_manager.cc
[zircon-component]: ../src/developer/debug/debug_agent/zircon_component_manager.cc
[protocol]: ../src/developer/debug/ipc/protocol.h
[async-rust]: ../src/developer/debug/zxdb/client/fuchsia_async_rust_task_provider.cc
[async-fdf]: ../src/developer/debug/zxdb/client/fdf_dispatcher_task_provider.cc
[format-exception]: ../src/developer/debug/zxdb/console/format_exception.cc
