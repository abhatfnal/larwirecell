#include "larwirecell/Components/RecoFramePayload.h"
#include "larwirecell/Components/RecoFrameProtocol.h"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

  int child_exit_status(pid_t const child)
  {
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    return WEXITSTATUS(status);
  }

  // Test 1: an invalid --output-mode value causes exit 2.
  // A socketpair is created so the fd argument names a valid OS file descriptor;
  // the binary should reject the invalid mode during argument parsing before
  // reading anything from the socket.
  void test_invalid_output_mode_exits_2(std::string const& worker)
  {
    int sockets[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    auto const child = ::fork();
    assert(child >= 0);
    if (child == 0) {
      ::close(sockets[0]);
      auto const flags = ::fcntl(sockets[1], F_GETFD);
      assert(flags >= 0);
      assert(::fcntl(sockets[1], F_SETFD, flags & ~FD_CLOEXEC) == 0);
      auto const fd = std::to_string(sockets[1]);
      ::execl(worker.c_str(), worker.c_str(),
              "--output-mode", "invalid_mode",
              "--ipc-fd", fd.c_str(),
              nullptr);
      ::_exit(127);
    }
    ::close(sockets[0]);
    ::close(sockets[1]);
    assert(child_exit_status(child) == 2);
  }

  // Test 2: a syntactically valid but non-existent fd (99999) with
  // --output-mode parent causes exit 2 when the binary tries to read from
  // the missing file descriptor.
  void test_output_mode_parent_rejects_bad_fd(std::string const& worker)
  {
    auto const child = ::fork();
    assert(child >= 0);
    if (child == 0) {
      ::execl(worker.c_str(), worker.c_str(),
              "--ipc-fd", "99999",
              "--output-mode", "parent",
              nullptr);
      ::_exit(127);
    }
    assert(child_exit_status(child) == 2);
  }

  // Test 3: omitting --ipc-fd entirely causes exit 2.
  void test_missing_ipc_fd_arg(std::string const& worker)
  {
    auto const child = ::fork();
    assert(child >= 0);
    if (child == 0) {
      ::execl(worker.c_str(), worker.c_str(),
              "--output-mode", "parent",
              nullptr);
      ::_exit(127);
    }
    assert(child_exit_status(child) == 2);
  }

} // namespace

int main(int const argc, char const* const* const argv)
{
  assert(argc == 2);
  std::string const worker{argv[1]};

  test_invalid_output_mode_exits_2(worker);
  test_output_mode_parent_rejects_bad_fd(worker);
  test_missing_ipc_fd_arg(worker);

  return 0;
}
