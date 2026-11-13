#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <string_view>

#include "src/developer/debug/debug_agent/linux_utils.h"

int main(int argc, char** argv) {
  if (argc < 2)
    return 1;
  std::string_view mode(argv[1]);
  if (mode == "--sigpipe" && argc > 3) {
    struct sigaction action{};
    action.sa_handler = std::string_view(argv[2]) == "ignored" ? SIG_IGN : SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, nullptr) != 0)
      return 1;
    execv(argv[3], argv + 3);
    return 1;
  }
  if (mode == "--auxv-child") {
    auto before = debug_agent::linux::ReadAuxv(getpid());
    if (before.empty() || chmod(argv[0], 0111) != 0)
      return 1;
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd >= 0) {
      close(fd);
      return 77;  // Privileged users can bypass the permission check.
    }
    if (errno != EACCES)
      return 1;
    return debug_agent::linux::ReadAuxv(getpid()) == before ? 0 : 1;
  }
  if (mode != "--auxv")
    return 1;

  // Change permissions only on a private copy, never on the build artifact.
  char path[] = "/tmp/zxdb-auxv-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return 1;
  close(fd);
  std::error_code error;
  std::filesystem::copy_file("/proc/self/exe", path,
                            std::filesystem::copy_options::overwrite_existing, error);
  if (error || chmod(path, 0700) != 0) {
    unlink(path);
    return 1;
  }
  pid_t child = fork();
  if (child == 0) {
    execl(path, path, "--auxv-child", nullptr);
    _exit(1);
  }
  int status = 0;
  pid_t waited = -1;
  if (child > 0) {
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
  }
  unlink(path);
  return waited > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
