# Linux debugger audit

Audited 2026-11-12 at commit `6e2fae0a98efa2bef8ae2816b5aa364aec2f8b37`.
Scope: the integrated Linux x86-64 agent, its PID/ptrace event loop, and the
shared process, thread, and breakpoint code used by that agent. This is an
audit and test plan; it does not implement fixes.

The Linux backend is not yet reliable for general multithreaded or
multiprocess debugging. The largest risks are lost wait events, incomplete
thread attachment, incorrect exec/exit transitions, and breakpoint operations
that assume a stop succeeded. A successful single-threaded session does not
exercise these paths.

## Evidence and priorities

- **Observed:** reproduced during this audit using the current libraries or
  executable. The reproduction method and its limits are stated below.
- **Code:** the implementation contains the defect or missing behavior. The
  described failure sequence has not necessarily been reproduced.
- **Test:** a plausible failure or unsupported scenario that needs a targeted
  experiment before being treated as a confirmed runtime defect.
- **P1:** can hang the debugger/target, lose control, corrupt target state, or
  report materially false state. **P2:** other correctness and support gaps.

Review covered the Linux-specific files in `debug_agent`, `MessageLoopLinux`,
local-agent startup/shutdown, and the common breakpoint/event paths. References
use Linux v6.12 and LLDB 19.1.7 where possible so the comparisons are stable.
They explain kernel behavior and provide implementation examples; LLDB's
policies are not automatically the right policies for zxdb.

### Focused checks performed

The message-loop probes linked the existing `debug_shared` library. Scratch
sources and binaries were kept outside the repository, under `/tmp`.

| Check | Method | Result |
| --- | --- | --- |
| Coalesced child exits | Block SIGCHLD through `MessageLoopLinux`; fork two children; use `waitid(WEXITED \| WNOWAIT)` to ensure both are waitable before running the loop | One callback instead of two; the other child still required a manual reap. See L01. |
| Stale notification | Consume a child's SIGSTOP with synchronous `waitpid(WUNTRACED)`, then run its registered signal watcher | Callback received status `0`, interpreted as normal exit, although the child was stopped and alive. See L02. |
| Signal-mask cleanup | Compare the calling thread's mask before constructing the loop and after cleanup/destruction | SIGCHLD changed from unblocked to blocked and stayed blocked. See L25. |
| Nonzero inferior exit | Launch a small program returning `7` through `build/zxdb --local` and the script runner | Console reported `Process 1 exited with code 1792.` See L09. |
| Inferior signal state | Inferior records its mask and SIGPIPE disposition; compare against direct execution with SIGPIPE reset to default | Direct: SIGCHLD unblocked, SIGPIPE default. Under zxdb: SIGCHLD blocked, SIGPIPE ignored. See L25. |
| Auxiliary-vector bounds | Compile unchanged `linux_utils.cc` with ASan; replace the file-reading helper with one that reads real `/proc/self/auxv` and shrinks the string allocation to its payload | Heap-buffer-overflow at `linux_utils.cc:232`. This demonstrates the parsing overread with an exact-sized allocation, not an observed crash of the stock executable. See L23. |
| Large map inode | Call the current `ParseMaps` with inode `4294967296`, followed by another valid map row | Only one entry returned, with a clamped inode and empty path. See L30. |
| Launch notification order | Observe both integrated launch probes | Both emitted the unexpected-process thread-start warning; source inspection establishes that the notification precedes the launch reply. See L29. |

The probes ran on an x86-64 Linux 6.12-based host. Integrated probes required
execution outside the sandbox because it blocks sockets/ptrace. The broad test
suite was not rerun for this documentation-only audit. No claim is made that
the timing-dependent scenarios below have all been reproduced.

## PID events, ptrace stops, and process lifetime

### L01 — SIGCHLD coalescing loses events [P1, Observed]

**Location:** [message_loop_linux.cc:52–64](src/developer/debug/shared/message_loop_linux.cc#L52-L64).

The loop reads one signalfd record, selects only the watch matching `ssi_pid`,
calls `waitpid` once, and returns. If two tasks stop or exit before the loop
runs, only one may receive a callback. The other can remain stopped forever or
remain a zombie. An unrelated/unwatched child can also supply the retained
signal and cause relevant task events to be missed.

SIGCHLD is a standard signal and does not queue once per event. Linux's
[`legacy_queue` / `__send_signal_locked`](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)
implements this coalescing. Treat the signal as a wakeup to drain owned wait
statuses, using each returned TID for dispatch. LLDB's
[`WaitPid` and `Manager::SigchldHandler`](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.7/lldb/source/Plugins/Process/Linux/NativeProcessLinux.cpp)
do that in a loop.

**Tests:** two already-waitable children; simultaneous breakpoint/exit events;
an unrelated child exiting first; mixed exit and clone events. Assert every
waitable status is consumed exactly once without needing another signal.

### L02 — waitpid returning zero fabricates an exit [P1, Observed]

**Location:** [message_loop_linux.cc:60–62](src/developer/debug/shared/message_loop_linux.cc#L60-L62);
[linux_task.cc:143–147](src/developer/debug/debug_agent/linux_task.cc#L143-L147).

The condition is `waitpid(...) >= 0`, with `status` initialized to zero. A
synchronous wait in launch or suspend can consume the status before the
pending signalfd notification is read. The later nonblocking wait returns zero
and the watcher receives a fabricated successful exit. The task dispatcher
ignores `WIFEXITED`, which can conceal this bug and plausibly explains the
nearby TODO about unexpected exits during attach. Implementing final-exit
handling without fixing this condition would make the failure more destructive.

Only a positive wait result identifies a status. EINTR also needs retry/error
handling rather than discarding the wakeup. See the
[`waitpid` return-value contract](https://man7.org/linux/man-pages/man2/waitpid.2.html).

**Tests:** consume launch, pause, and detach stops synchronously before reading
signalfd; inject wait results `0`, `EINTR`, and `ECHILD`; assert no invented event.

### L03 — clone/fork parent and child stops are not joined [P1, Code]

**Location:** [linux_task.cc:169–216](src/developer/debug/debug_agent/linux_task.cc#L169-L216);
[linux_process_handle.cc:322–333](src/developer/debug/debug_agent/linux_process_handle.cc#L322-L333).

The parent event immediately creates an attached child task and increments its
suspend count. There is no wait for the child's initial stop. Observer setup,
memory patching, and resume can run before the child has entered that stop.
Conversely, a child notification arriving before registration is
discarded by the message loop. Registering a watch does not drain an already
waitable status. These are additional problems even after L01 is fixed.

The child stop and parent creation event can arrive in either order. LLDB's
[`Manager::CollectThread`](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.7/lldb/source/Plugins/Process/Linux/NativeProcessLinux.cpp)
joins them and retains early unowned child stops.

**Tests:** force both orders with barriers or injected events; immediate child
exit; SIGKILL between creation and collection; fork with installed breakpoints.
Require one child object, one creation notification, and no premature resume.

### L04 — attach does not attach existing nonleader threads [P1, Code]

**Location:** [linux_process_handle.cc:73–100](src/developer/debug/debug_agent/linux_process_handle.cc#L73-L100);
[debugged_process.cc:278–291](src/developer/debug/debug_agent/debugged_process.cc#L278-L291).

`Attach` seizes only the leader. Enumeration wraps other TIDs in new, unattached
`LinuxTask` objects without observers. They appear in the thread list but cannot
reliably be stopped, read, or single-stepped. A worker can hit a process-wide
INT3 without being traced. Threads created by unattached workers are also
missed. A single enumeration cannot close the attach-versus-creation race.

Ptrace attachment is per thread. LLDB's
[`Attach`](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.7/lldb/source/Plugins/Process/Linux/NativeProcessLinux.cpp)
repeatedly enumerates and attaches TIDs until the set stabilizes.

**Tests:** attach to existing workers; create/exit workers during attach; place
a breakpoint hit only by a worker. Inspect every TID's `TracerPid` and verify
register access and all-stop behavior for each one.

### L05 — a CLONE event is assumed to identify a thread [P1, Code]

**Location:** [linux_task.cc:169–183](src/developer/debug/debug_agent/linux_task.cc#L169-L183).

Every CLONE event calls `OnThreadStarting`. A clone-created process can instead
be added to the parent's thread map, causing memory and module operations to
use the wrong address space. The implementation needs the child's TGID and an
explicit policy for shared address spaces, not only the event name.

Linux chooses FORK/CLONE/VFORK events from clone flags and exit-signal rules in
[`kernel_clone`](https://github.com/torvalds/linux/blob/v6.12/kernel/fork.c).
`PTRACE_EVENT_CLONE` does not imply `CLONE_THREAD`.

**Tests:** pthreads; clone/clone3 without `CLONE_THREAD` and with exit signal
zero or SIGUSR1; `CLONE_VM` without `CLONE_THREAD`. Verify process ownership,
breakpoint ownership, and detach behavior.

### L06 — vfork handling is empty despite enabling it [P1, Code]

**Location:** [linux_task.cc:28–29](src/developer/debug/debug_agent/linux_task.cc#L28-L29)
and [219–222](src/developer/debug/debug_agent/linux_task.cc#L219-L222).

`PTRACE_O_TRACEVFORK` is enabled, but `OnVFork` neither handles the child nor
resumes the parent. This can hang direct vfork and library spawning paths.
There is no VFORK_DONE handling. Copying the ordinary fork path is insufficient:
its breakpoint restoration writes must account for shared parent/child memory.

**Tests:** vfork, posix_spawn, and system; child exec success/failure and exit
without exec; breakpoints present in the shared image. LLDB's
[`MonitorClone` and VFORK_DONE handling](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.7/lldb/source/Plugins/Process/Linux/NativeProcessLinux.cpp)
are useful comparisons for breakpoint restoration timing.

### L07 — exec does not reset identity or image state [P1, Code]

**Location:** [linux_task.cc:36–41](src/developer/debug/debug_agent/linux_task.cc#L36-L41)
and [161–167](src/developer/debug/debug_agent/linux_task.cc#L161-L167);
[debugged_process.cc:153–180](src/developer/debug/debug_agent/debugged_process.cc#L153-L180).

EXEC is omitted from `HasEventMessage`. `OnExec` emits a generic zero-siginfo
exception; it does not migrate the execing thread's identity, remove obsolete
siblings, reset modules/loader addresses, or discard old breakpoint bytes and
step-over state. Even leader exec changes the address space. Worker exec also
changes the TID to the TGID, so a map keyed only by the old numeric TID is wrong.
L08 can tear down the process before the replacement image is handled.

See the [ptrace exec rules](https://man7.org/linux/man-pages/man2/ptrace.2.html):
GETEVENTMSG provides the former TID, and only one thread remains after exec.

**Tests:** leader exec; worker exec; simultaneous execs; worker exec after leader
exit; exec during step-over; PIE-to-non-PIE and ABI changes. Assert one surviving
thread, correct new modules, and no writes using old breakpoint state.

### L08 — pre-exit, thread death, process death, and reaping are conflated [P1, Code]

**Location:** [linux_task.cc:143–149](src/developer/debug/debug_agent/linux_task.cc#L143-L149),
[224–247](src/developer/debug/debug_agent/linux_task.cc#L224-L247),
[366–369](src/developer/debug/debug_agent/linux_task.cc#L366-L369);
[linux_process_handle.cc:311–337](src/developer/debug/debug_agent/linux_process_handle.cc#L311-L337);
[debug_agent.cc:277–304](src/developer/debug/debug_agent/debug_agent.cc#L277-L304).

At PTRACE_EVENT_EXIT, the task sets `exiting_`. A leader event immediately calls
`OnProcessTerminated`, although other threads can still live after the leader
calls `pthread_exit`. A nonleader terminal signal also unconditionally reports
process termination. `DecrementSuspendCount` refuses to continue an exiting
task, leaving progress dependent on eventual destructor detach. Final
`WIFEXITED` is ignored, and removing the process removes its watches before
final child reaping. The kill path also removes the process immediately, even
after a kill error; the Linux kill TODO already reports zombies.

Linux [`do_exit`](https://github.com/torvalds/linux/blob/v6.12/kernel/exit.c)
emits the ptrace exit stop before final exit cleanup. The agent needs separate
pre-exit and terminal/reaped states and a remaining-thread policy. A final
wait obligation can outlive the frontend's process object.

**Tests:** leader pthread_exit with live workers; worker exit_group; kill at a
breakpoint; kill during clone/exec; repeated launch/exit and detach/exit. Check
frontend events and `/proc`/wait state for zombies, stopped orphans, and
premature loss of live workers.

### L09 — exit status is not decoded [P2, Observed]

**Location:** [linux_task.cc:224–235](src/developer/debug/debug_agent/linux_task.cc#L224-L235);
[linux_process_handle.h:36](src/developer/debug/debug_agent/linux_process_handle.h#L36).

The raw event message is returned as the program's exit code. Returning 7
reports 1792. Zero-exit tests hide this. Signal termination and core-dump state
also need a defined client representation. Linux's
[`exit` / `exit_group` encoding](https://github.com/torvalds/linux/blob/v6.12/kernel/exit.c)
must be decoded before constructing the notification.

**Tests:** exits 1, 7, 127, 255; normal final exit without a pre-exit event;
SIGTERM, SIGSEGV, and SIGKILL. Verify exactly one terminal notification.

### L10 — failed stop/resume operations still change logical state [P1, Code]

**Location:** [linux_task.cc:292–384](src/developer/debug/debug_agent/linux_task.cc#L292-L384);
[linux_thread_handle.cc:31–34](src/developer/debug/debug_agent/linux_thread_handle.cc#L31-L34).

Suspend count increments before tkill/waitpid. Failure leaves a positive count,
and `WaitForSuspension` always returns true. An unattached task also acquires a
logical suspend without stopping. The blocking wait has no deadline and no
EINTR retry. Resume decrements the count before ptrace success and only logs a
failure. Thus “stopped” and “running” can disagree with the kernel, and a single
blocked task can freeze all agent requests. Failed GETEVENTMSG in clone/fork
also only logs and leaves the event unresolved.

**Tests:** inject EINTR/ESRCH/ECHILD and resume/event-message errors; exit or
SIGKILL during pause; attach races; an uninterruptible task. Failed transitions
must remain visible and must not authorize memory/register operations.

### L11 — delayed debugger SIGSTOP can stop a resumed or detached inferior [P1, Code]

**Location:** [linux_task.cc:98–108](src/developer/debug/debug_agent/linux_task.cc#L98-L108)
and [309–356](src/developer/debug/debug_agent/linux_task.cc#L309-L356).

If a breakpoint or clone stop is already pending, suspend sends SIGSTOP but
waitpid consumes that other stop. The code posts the other event and forgets
that its own SIGSTOP is still pending. Continuing can produce a spurious
exception. Detaching can instead leave an untraced target group-stopped.
The SIGSTOP match also does not distinguish an external stop from our request.

LLDB's [`MonitorSignal` and `Detach`](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.7/lldb/source/Plugins/Process/Linux/NativeProcessLinux.cpp)
track self-generated stops and account for pending stop signals during detach.
Any equivalent policy here must preserve intentional external job-control stops.

**Tests:** pause/detach while another stop is already waitable; external SIGSTOP
racing a debugger pause; repeated breakpoint/pause/continue. After detach,
independently verify expected liveness and `/proc` state.

### L12 — signal forwarding is a successful no-op [P1, Code]

**Location:** [linux_exception_handle.cc:52–73](src/developer/debug/debug_agent/linux_exception_handle.cc#L52-L73);
[linux_task.cc:104](src/developer/debug/debug_agent/linux_task.cc#L104)
and [375–380](src/developer/debug/debug_agent/linux_task.cc#L375-L380).

`SetResolution` returns success without recording the request. All continue,
single-step, and detach calls pass signal zero. Handlers therefore do not
receive forwarded signals; synchronous faults can repeat indefinitely. The
exception record is empty, so the client loses signal number, origin, si_code,
and fault address. Non-SIGTRAP signals all decode as unknown.

Linux's [`ptrace_signal`](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)
suppresses delivery when the tracer resumes with zero. Synthetic events and
real signal-delivery stops must remain distinct through resume.

**Tests:** SIGUSR1/SIGTERM handlers, ignored signals, fatal faults, queued
real-time signals, and program-generated SIGTRAP. Check handler counts and
siginfo for both resolve and forward, including while single-stepping.

### L13 — group-stop and seized-task event-stop semantics are absent [P1, Code]

**Location:** [linux_task.cc:132–157](src/developer/debug/debug_agent/linux_task.cc#L132-L157)
and [250–286](src/developer/debug/debug_agent/linux_task.cc#L250-L286).

There is no PTRACE_EVENT_STOP branch or PTRACE_LISTEN policy. GETSIGINFO failure
is ignored without distinguishing EINVAL from ESRCH. Synthetic initial stops
and job-control stops can become ordinary exceptions or be left unresolved.
The message-loop wait flags omit WCONTINUED despite having an `OnContinued`
branch; decide whether continuation events are required rather than assuming
that branch is functional.

The [ptrace stop descriptions](https://man7.org/linux/man-pages/man2/ptrace.2.html)
distinguish signal-delivery, group, and event stops. TRACEME launch and SEIZE
attach must both have explicit handling.

**Tests:** attach to an already stopped process; SIGSTOP/SIGCONT and
SIGTSTP/SIGCONT cycles; background terminal I/O; pause during a group stop.

### L14 — deferred events lack attachment/observer validity checks [P1, Test]

**Location:** [linux_task.cc:111–127](src/developer/debug/debug_agent/linux_task.cc#L111-L127)
and [337–352](src/developer/debug/debug_agent/linux_task.cc#L337-L352).

A queued `SignalRecord` checks only the task's weak pointer. The task can
outlive its observer through exception/suspend references. Detach or observer
removal does not invalidate the queued event by attachment generation. Replay
can attempt an observer call after the relationship ended, or apply an old
stop to a changed state. A concrete crash has not been shown. Raw PID/TID reuse
after reaping needs the same stale-event defense; a numeric PID is not a
permanent object identity.

**Tests:** queue a non-SIGSTOP event during suspend, then detach/remove the
observer while retaining a task reference; reattach before replay; inject PID
reuse and terminal events. Verify no stale callback, signal, or memory write.

## Breakpoints, stepping, and registers

### L15 — exceptional step-over completion can strand peers and traps [P1, Code]

**Location:** [debugged_thread.cc:175–205](src/developer/debug/debug_agent/debugged_thread.cc#L175-L205);
[software_breakpoint.cc:146–214](src/developer/debug/debug_agent/software_breakpoint.cc#L146-L214);
[debugged_process.cc:544–557](src/developer/debug/debug_agent/debugged_process.cc#L544-L557).

Software step-over removes the trap and holds peer suspend tokens. Cleanup is
performed on a single-step exception only; the source contains an explicit TODO
for other exceptions. A signal/fault, exec, or stepping-thread exit can leave
the trap absent and the queue blocked. A fault might eventually complete after
another continue, but there is no complete recovery policy for all alternate
events, especially thread removal.

**Tests:** break on a faulting instruction, clone/exec syscall, and raw
thread-exit syscall; inject a signal or kill the stepping worker; queue another
worker at the same and at another breakpoint. Check patched bytes, queue
progress, and every peer's suspend tokens after each outcome.

### L16 — new threads can escape the breakpoint step-over stop set [P1, Code]

**Location:** [software_breakpoint.cc:216–240](src/developer/debug/debug_agent/software_breakpoint.cc#L216-L240);
[debugged_process.cc:200–223](src/developer/debug/debug_agent/debugged_process.cc#L200-L223)
and [516–542](src/developer/debug/debug_agent/debugged_process.cc#L516-L542).

Step-over suspends a snapshot of known threads. A new thread is suspended only
if `suspend_new_threads_` is set, and resume clears that flag even for a single
thread. A clone created by the stepping instruction, or a queued clone event
from a peer, can add a runnable thread while the breakpoint is unpatched.
This remains a separate gap after child-stop synchronization in L03 is fixed;
the current premature-resume failures can mask it.
The local `suspended_threads` vector is never populated, so the apparent wait
loop in `SuspendAllOtherThreads` executes zero times. Linux's claimed
synchronous suspension does not make its error paths safe (L10).

**Tests:** hold the parent at clone while child creation completes; verify the
child cannot pass an unpatched breakpoint. Add threads during an all-stop and
during single-thread resume, and verify the intended policy.

### L17 — instruction stepping from a breakpoint may execute an extra instruction [P2, Test]

**Location:** [debugged_thread.cc:175–237](src/developer/debug/debug_agent/debugged_thread.cc#L175-L237);
[software_breakpoint.cc:181–214](src/developer/debug/debug_agent/software_breakpoint.cc#L181-L214).

After the internal step that executes the original instruction, the breakpoint
branch cleans up and resumes before reaching instruction-count/range handling.
In instruction-step mode this appears to request another step. This is a
control-flow finding, not a runtime-proven off-by-one.

**Tests:** assembly with two labeled instructions and separate side effects;
compare `stepi 1` from a software breakpoint against stepping from a pause.
Check exact PC, counts greater than one, and leaving a requested range.

### L18 — register APIs are stubs and can invalidate breakpoint bookkeeping [P1, Code]

**Location:** [linux_thread_handle.cc:54–90](src/developer/debug/debug_agent/linux_thread_handle.cc#L54-L90);
[debugged_thread.cc:505–530](src/developer/debug/debug_agent/debugged_thread.cc#L505-L530);
[debug_agent.cc:429–435](src/developer/debug/debug_agent/debug_agent.cc#L429-L435).

Protocol register reads/writes return empty vectors. The reply can still report
success. General registers carried in stack records mask part of the missing
API, but explicit reads and floating-point/vector state remain unavailable.
A proposed RIP write clears `current_breakpoint_` before the no-op write, so
the debugger can lose the breakpoint relation without changing RIP. The
separate native `SetGeneralRegisters` path discards errors as well; failed RIP
correction must not be treated as successful.

**Tests:** read/write every register category; RIP change at a breakpoint;
injected SETREGSET failure. Require explicit unsupported/error responses and
commit bookkeeping only after a successful write.

### L19 — hardware breakpoints/watchpoints are advertised but absent [P2, Code]

**Location:** [linux_thread_handle.cc:66–112](src/developer/debug/debug_agent/linux_thread_handle.cc#L66-L112);
[linux_arch_x64.cc:19–27](src/developer/debug/debug_agent/linux_arch_x64.cc#L19-L27).

Debug-register access and installation/removal operations return false or
empty values, while system information reports four breakpoint/watchpoint
slots. Watchpoint traps lack DR6-based classification. New-thread handling
does not install existing hardware definitions in the child.

**Tests:** truthful rejection now; when implemented, shared slot allocation,
alignment/ranges, execute/write/read-write combinations, clone inheritance,
per-thread targeting, detach cleanup, and simultaneous step/watchpoint causes.

### L20 — TRAP_BRKPT is not uniquely a syscall single-step [P2, Code]

**Location:** [linux_arch_x64.cc:57–75](src/developer/debug/debug_agent/linux_arch_x64.cc#L57-L75);
[debugged_thread.cc:209–217](src/developer/debug/debug_agent/debugged_thread.cc#L209-L217).

Every TRAP_BRKPT becomes `kSingleStep`, and an unexpected single-step in continue
mode is resumed silently. Linux can emit that code for ICEBP/INT1 as well as
some single-step reports. A program's trap can therefore disappear. Classification
needs the pending step state and architecture context, not just this code.

See Linux's [`exc_debug_user`](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/traps.c),
[`get_si_code`](https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/traps.h),
and [`user_single_step_report`](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/ptrace.c).

**Tests:** debugger INT3, hardcoded INT3, ICEBP byte `0xf1`, raise(SIGTRAP),
user-set TF, and single-stepping a syscall. Verify PC adjustment and reason.

### L21 — inferior register ABI and returned regset size are unchecked [P1, Code]

**Location:** [linux_utils.cc:74–92](src/developer/debug/debug_agent/linux_utils.cc#L74-L92).

GETREGSET fills an uninitialized native `user_regs_struct`; returned `iov_len`
is ignored. A compat inferior can provide a smaller, differently laid-out
regset, yielding invalid values and uninitialized tail data. A host x86-64
build does not establish that every launched executable is x86-64.

Linux's [`task_user_regset_view`](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/ptrace.c)
selects the tracee ABI. Reject unsupported ABIs explicitly until supported.

**Tests:** i386 executable, x86-64-to-i386 exec, short regset result, and target
death during access. Verify no native-layout interpretation of compat data.

### L22 — failed trap removal/reinstallation can be reported as success [P1, Code]

**Location:** [software_breakpoint.cc:91–126](src/developer/debug/debug_agent/software_breakpoint.cc#L91-L126),
[146–175](src/developer/debug/debug_agent/software_breakpoint.cc#L146-L175);
[debugged_process.cc:98–132](src/developer/debug/debug_agent/debugged_process.cc#L98-L132)
and [590–604](src/developer/debug/debug_agent/debugged_process.cc#L590-L604).

Removal marks the breakpoint uninstalled even when restoration fails.
`UninstallFromMemorySpace` returns success after a failed write; step-over
ignores uninstall/update results; fork-child restoration also ignores results.
A live target can retain an INT3 after detach, or later run without a supposedly
installed breakpoint. This compounds premature child operations in L03.
An unmapped/dead address must be distinguished from a live-target access failure.

**Tests:** injected failures at install/remove/reinsert/detach; unmap/remap a
breakpoint address; fork with breakpoints. Verify actual bytes before reporting
success or resuming untraced execution. Also test multiword writes and edge-word
read/modify/write while peers run: Linux WriteMemory stops only the leader.

## Memory, launch, shutdown, and platform gaps

### L23 — auxiliary-vector parsing reads past the payload [P1, Observed with ASan harness]

**Location:** [linux_utils.cc:224–234](src/developer/debug/debug_agent/linux_utils.cc#L224-L234).

`key_count` divides bytes by one 64-bit word, but each iteration reads two
words. The parser reads twice the payload extent and does not stop at AT_NULL.
It can import unrelated heap contents into the map or fault. Spare capacity
in the normal file reader can hide the overread from ASan; the exact-sized
allocation probe exposed it. Alignment, malformed lengths, and inferior word
size also require a deliberate parsing contract.

**Tests:** exact-sized valid auxv; empty/truncated input; AT_NULL; long input;
32-bit ABI rejection. Exercise loader discovery under ASan/UBSan.

### L24 — partial memory reads manufacture valid zero-filled bytes [P1, Code]

**Location:** [linux_process_handle.cc:146–196](src/developer/debug/debug_agent/linux_process_handle.cc#L146-L196).

`ReadMemory` reports the actual byte count, but `ReadMemoryBlocks` ignores it
when the call succeeds. A read crossing into unmapped memory can return a valid
block of the requested size with an unread zero-filled tail. Variable values,
disassembly, and unwinding can use invented data. There is no fallback when
process_vm_readv cannot access memory that ptrace could read.

**Tests:** page boundary into a hole/PROT_NONE mapping; partial reads; zero size;
address overflow; denied reads; mappings removed concurrently. Require exact
valid/invalid block ranges and initialized `actual` values on every path.

### L25 — launch leaks debugger signal state into the inferior [P1, Observed]

**Location:** [message_loop_linux.cc:21–35](src/developer/debug/shared/message_loop_linux.cc#L21-L35);
[linux_binary_launcher.cc:136–174](src/developer/debug/debug_agent/linux_binary_launcher.cc#L136-L174);
[main.cc:121](src/developer/debug/zxdb/main.cc#L121).

The loop blocks SIGCHLD, and main ignores SIGPIPE. The launch child does not
restore a target signal mask/disposition before exec. Programs using SIGCHLD
handlers or normal SIGPIPE behavior therefore behave differently under zxdb.
The loop never restores its caller's mask. Blocking in the constructor is
inconsistent with the base API allowing construction on one thread and Init/Run
on another. Existing unblocked helper threads can consume signals instead of
the intended signalfd.

Masks and ignored dispositions survive exec as described in
[`signal(7)`](https://man7.org/linux/man-pages/man7/signal.7.html).
Signalfd setup also ignores failure, lacks NONBLOCK/CLOEXEC, and checks reads
only with a debug assertion. Audit initialization failure and ownership using
the [`signalfd` contract](https://man7.org/linux/man-pages/man2/signalfd.2.html).

**Tests:** child with a SIGCHLD handler and child with a closed pipe; preexisting
mask preservation; loop construction on another thread; descriptor exhaustion;
read errors; repeated Init/Cleanup. Define which user-inherited signal settings
should be preserved separately from debugger-added settings.

### L26 — launcher failures can consume the wrong child or leave an orphan [P1, Code]

**Location:** [linux_binary_launcher.cc:109–120](src/developer/debug/debug_agent/linux_binary_launcher.cc#L109-L120)
and [136–218](src/developer/debug/debug_agent/linux_binary_launcher.cc#L136-L218);
[local_agent.cc:55–82](src/developer/debug/zxdb/local_agent.cc#L55-L82).

Inferior `fork()` failure falls into the parent branch, followed by
`waitpid(-1, ...)`, which can block on or consume another inferior's event.
TRACEME's result is unchecked, so a child may run untraced. The initial wait
does not retry EINTR; unexpected-stop/error paths have no explicit kill/reap
ownership. Pipe creation failures leak previously opened descriptors. Local
agent pipe/fork setup has similar partial-allocation cleanup gaps.

**Tests:** injected fork/pipe/ptrace failures and EINTR; missing/non-executable
program and invalid interpreter; SIGKILL or another signal before exec trap;
closed standard descriptors. Require no unrelated wait consumption, FD leak,
or surviving unowned child and a useful launch error.

### L27 — detach and frontend shutdown do not guarantee safe target release [P1, Code/Test]

**Location:** [linux_task.cc:98–108](src/developer/debug/debug_agent/linux_task.cc#L98-L108);
[debugged_process.cc:98–132](src/developer/debug/debug_agent/debugged_process.cc#L98-L132);
[main.cc:173–179](src/developer/debug/zxdb/main.cc#L173-L179);
[local_agent.cc:38–51](src/developer/debug/zxdb/local_agent.cc#L38-L51).

Detach clears `is_attached_` even when PTRACE_DETACH fails. Common teardown
resumes threads before detaching and does not maintain one confirmed stop of
all affected threads throughout breakpoint restoration. Nonleader teardown
depends on object lifetimes. Deferred events and delayed SIGSTOP complicate
this interval (L11/L14).

Frontend cleanup unconditionally kills the local agent and waits for it. It
can race the agent's normal cleanup after pipe closure. Kernel tracer exit
does not restore debugger-patched instructions. Whether normal quit happens
to finish all detach work first needs a dedicated stress test. Abrupt frontend
or agent death needs an explicit target-survival policy; EXITKILL is a policy
option, not a substitute for graceful cleanup.

**Tests:** quit/detach/disconnect with targets running, stopped, and stepping;
multiple threads/processes; injected detach failure; frontend and agent crash.
Check target liveness, signal state, TracerPid, trap bytes, and reaping externally.

### L28 — process exit can discard unread stdout/stderr [P2, Code]

**Location:** [debugged_process.cc:503–513](src/developer/debug/debug_agent/debugged_process.cc#L503-L513)
and [692–767](src/developer/debug/debug_agent/debugged_process.cc#L692-L767).

The exit callback immediately deletes the process and its stream watchers. If
the exit notification is handled before a ready output FD, unread bytes are
lost. BufferedFD's own EOF draining cannot help once its owner is deleted.
The first fast-exit probe showed no output, while a version delaying exit did;
that observation is consistent with this race but does not isolate it.

**Tests:** write stdout/stderr sentinels then immediately _exit; large output
and backpressure; final partial lines; fork children retaining output FDs.
Verify byte delivery and define ordering relative to process exit.

### L29 — thread notifications precede process creation in the client [P2, Observed]

**Location:** [debugged_process.cc:179–180](src/developer/debug/debug_agent/debugged_process.cc#L179-L180),
[278–291](src/developer/debug/debug_agent/debugged_process.cc#L278-L291);
[debug_agent.cc:1033–1060](src/developer/debug/debug_agent/debug_agent.cc#L1033-L1060)
and [1195–1205](src/developer/debug/debug_agent/debug_agent.cc#L1195-L1205).

Linux initialization populates and announces threads before the launch reply
or process-start notification establishes the process in the client. Both
launch probes emitted the unexpected-process warning. Later syncing can hide
the missing initial notification. Fork follows the same ordering risk.

**Tests:** assert process-before-thread notification order for launch, PID
attach, and fork; immediate exception/exit after creation; no frontend warning
or dependence on a later refresh to recover the thread.

### L30 — procfs and loader discovery need stronger parsing and coverage [P2, Observed/Code/Test]

**Location:** [linux_utils.h:43](src/developer/debug/debug_agent/linux_utils.h#L43);
[linux_utils.cc:95–128](src/developer/debug/debug_agent/linux_utils.cc#L95-L128),
[190–220](src/developer/debug/debug_agent/linux_utils.cc#L190-L220),
[331–360](src/developer/debug/debug_agent/linux_utils.cc#L331-L360);
[elf_utils.cc](src/developer/debug/debug_agent/elf_utils.cc).

- **Observed:** map inode is an `int`. A valid inode exceeding INT_MAX sets
  stream failure and truncates map parsing. The large-inode probe lost the path
  and following map row.
- **Code:** procfs read failure is often collapsed to an empty result or a dead
  thread, obscuring permission/race errors.
- **Test:** loader lookup opens the tracee's mapped pathname in the debugger's
  namespace and assumes `_r_debug`. Exercise chroot/mount namespaces, deleted
  files, musl, static binaries, and loader-symbol absence.
- **Test:** module-list walks need validation against short reads and loader
  transitions; `WalkElfModules` does not consistently check actual read lengths
  or use `r_debug.r_state` to establish a consistent snapshot. Exercise dlopen,
  dlclose, dlmopen namespaces, and concurrent inspection.
- **Test:** `MergeMmapedModules` carries `vaddr_start` across module iterations.
  Validate load biases with mixed ET_EXEC/ET_DYN mappings instead of assuming
  the common PIE case establishes correctness.

**Tests:** parser fixtures with large inodes and malformed/truncated maps;
real loader lifecycle fixtures; short-read injection; renamed/deleted objects;
attach and inspect while threads change mappings.

### Additional explicit limitations

These are separate from broken implemented behavior. They do not all need to
be implemented for an initial release, but unsupported requests should fail
clearly.

| Area | Current limitation / test needed | Location |
| --- | --- | --- |
| System information | CPU count and physical memory size are placeholders | [linux_system_interface.cc:14–23](src/developer/debug/debug_agent/linux_system_interface.cc#L14-L23) |
| Core files | Reading minidumps is separate from saving a Linux process minidump, which is unimplemented | [linux_process_handle.cc:199–201](src/developer/debug/debug_agent/linux_process_handle.cc#L199-L201) |
| Job/component/limbo model | Fuchsia concepts do not map directly to Linux; component operations and job observation are incomplete. Existing-process filters and observation of future unrelated processes need separate verification | [linux_job_handle.cc](src/developer/debug/debug_agent/linux_job_handle.cc), [linux_component_manager.cc](src/developer/debug/debug_agent/linux_component_manager.cc), [linux_limbo_provider.h](src/developer/debug/debug_agent/linux_limbo_provider.h) |
| Fork policy | All observed fork children are attached unconditionally; no complete follow-parent/follow-child/detach-other policy | [debug_agent.cc:1126–1129](src/developer/debug/debug_agent/debug_agent.cc#L1126-L1129) |
| Attach priorities | Linux Attach ignores `AttachConfig`, including observation versus claiming an exception channel | [linux_process_handle.cc:98–100](src/developer/debug/debug_agent/linux_process_handle.cc#L98-L100) |
| Launch environment and terminal | Uses execv, requires an executable path, redirects stdin to /dev/null, and has no inferior PTY/job-control setup. Verify/document cwd, environment, stdin, Ctrl-C, and process-group behavior | [linux_binary_launcher.cc](src/developer/debug/debug_agent/linux_binary_launcher.cc) |
| Architectures | Build is Linux x86-64; ARM64 implementation has stubs and compat inferior support is not established | [linux_arch_arm64.cc](src/developer/debug/debug_agent/linux_arch_arm64.cc), L21 |
| Exception metadata | Process-handle recovery and exception record/resolution are incomplete | [linux_exception_handle.cc:43–83](src/developer/debug/debug_agent/linux_exception_handle.cc#L43-L83) |
| Observability | Local-agent debug logging requires changing a disabled source block; frontend --debug does not expose the agent event trace | [local_agent.cc:22–29](src/developer/debug/zxdb/local_agent.cc#L22-L29) |

## Required testing strategy

Backend coverage is much smaller than the total test count suggests:

- [MessageLoopLinux.SigChild](src/developer/debug/shared/message_loop_linux_unittest.cc)
  tests one untraced child exit.
- [The Linux agent test target](src/developer/debug/zxdb/Tests.cmake#L302-L310)
  includes alignment, ELF/Linux utilities, and descriptor shuffling. It does
  not include existing general debugged-thread/software-breakpoint tests or
  a LinuxTask ptrace lifecycle suite.
- [local_debugging](tests/local.script.in) launches one single-threaded
  inferior, hits one breakpoint, reads a variable, performs source `next`, and
  exits zero. It does not establish attach, signal, fork, exec, or timing safety.

Add three complementary test layers:

1. **Deterministic event/state tests.** Introduce a narrow wait/ptrace boundary
   that can supply exact events and failures. Cover reversed parent/child order,
   stale notifications, duplicate stops, interrupted waits, observer removal,
   and rollback after failed resume/detach. Keep raw wait status, event message,
   and siginfo together so tests can verify they belong to the same stop.
2. **Real-kernel integration fixtures.** Use pipes/barriers and small programs
   with threads, signals, forks, and execs. Run each applicable scenario through
   both TRACEME launch and SEIZE attach. Use a separate watchdog/reaper so a
   wedged agent cannot leave test processes behind. Record kernel/ABI details.
3. **Scheduler stress and sanitizers.** Repeat validated scenarios with CPU
   affinity changes and concurrent create/exit/pause operations. Run parser and
   lifetime code under ASan/UBSan. Stress supplements deterministic assertions;
   a timeout-free run alone does not establish correctness.

Cross **current state** (running, user-paused, internal stop requested,
breakpoint stop, single-step, detaching, pre-exit) with **next event** (signal,
group stop, clone/fork/vfork, exec, exit, SIGKILL). For each combination verify
TID/TGID identity, raw/decoded reason, PC, signal delivery count, trap bytes,
resume count, frontend notification order, and final wait ownership. Include
two simultaneous events from different tasks.

## Suggested implementation order and invariants

1. Fix L01/L02 and establish one owner of wait-status collection, including
   synchronous launch/stop paths. Drain independently of `ssi_pid`. Preserve
   unknown child stops until creation can associate them. If a global
   `waitpid(-1)` loop is used, explicitly own all child reaping in that process.
2. Replace implicit assumptions in suspend counts with explicit per-TID kernel
   states and pending stop requests. Counts express why execution is prohibited;
   they do not prove a kernel stop. Propagate errors and retain attachment
   generations for deferred work.
3. Complete thread attachment and creation synchronization, then exec and exit
   transitions. Never issue ptrace operations on a new child before collecting
   its stop; never treat a pre-exit event as a final reap.
4. Add signal resolution and job-control policy, then make step-over cleanup
   correct for every intervening event and for newly created threads. Keep
   breakpoint memory ownership tied to an address-space generation across exec
   and shared-memory creation.
5. Fix auxiliary-vector and memory-read defects immediately alongside this
   work. Make launch/shutdown cleanup explicit, then fill in register/HW
   support and broader loader/platform compatibility.

Every fix should add the smallest test that demonstrates its failure sequence.
The first release criterion should be correct stop ownership, signal delivery,
and target cleanup under these event combinations, rather than a larger count
of passing frontend tests.
