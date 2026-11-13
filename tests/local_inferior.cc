// A small DWARF fixture for the integrated Linux debugger smoke test.
__attribute__((noinline)) int StandaloneCompute(int value) {
  volatile int result = value + 7;
  return result;
}

#include <signal.h>
#include <string.h>

int main(int argc, char** argv) {
  sigset_t mask;
  struct sigaction action{};
  sigprocmask(SIG_SETMASK, nullptr, &mask);
  sigaction(SIGPIPE, nullptr, &action);
  bool expect_ignored = argc > 1 && strcmp(argv[1], "ignored") == 0;
  if (sigismember(&mask, SIGCHLD) ||
      action.sa_handler != (expect_ignored ? SIG_IGN : SIG_DFL))
    return 99;
  return StandaloneCompute(35) == 42 ? (argc > 1 ? 7 : 0) : 1;
}
