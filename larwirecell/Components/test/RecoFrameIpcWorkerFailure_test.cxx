#include "larwirecell/Components/RecoFramePayload.h"
#include "larwirecell/Components/RecoFrameProtocol.h"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

  wcls::RecoFramePayload payload()
  {
    wcls::RecoFramePayload result;
    result.identity = {1U, 2U, 3U};
    result.frame_ident = 3;
    result.frame_time_value = 0.0;
    result.tick_period_ns = 500.0;
    result.nticks = 4U;
    result.frame_tags = {"orig"};
    wcls::SparseTraceSet traces;
    traces.trace_tag = "test";
    traces.channels = {7U};
    traces.trace_range_offsets = {0U, 1U};
    traces.range_begin_tick = {1U};
    traces.range_value_offsets = {0U, 2U};
    traces.values = {1.0F, -0.0F};
    result.trace_sets.push_back(std::move(traces));
    wcls::validate_reco_frame_payload(result);
    return result;
  }

  void set_le16(std::vector<std::uint8_t>& bytes,
                std::size_t const offset,
                std::uint16_t const value)
  {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1U) = static_cast<std::uint8_t>(value >> 8U);
  }

  void set_le32(std::vector<std::uint8_t>& bytes,
                std::size_t const offset,
                std::uint32_t const value)
  {
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
      bytes.at(offset + shift / 8U) = static_cast<std::uint8_t>(value >> shift);
    }
  }

  void set_le64(std::vector<std::uint8_t>& bytes,
                std::size_t const offset,
                std::uint64_t const value)
  {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
      bytes.at(offset + shift / 8U) = static_cast<std::uint8_t>(value >> shift);
    }
  }

  pid_t spawn(std::string const& worker, int const parent_fd, int const child_fd)
  {
    auto const child = ::fork();
    assert(child >= 0);
    if (child == 0) {
      ::close(parent_fd);
      auto const flags = ::fcntl(child_fd, F_GETFD);
      assert(flags >= 0);
      assert(::fcntl(child_fd, F_SETFD, flags & ~FD_CLOEXEC) == 0);
      auto const fd = std::to_string(child_fd);
      ::execl(worker.c_str(), worker.c_str(), "--ipc-fd", fd.c_str(), nullptr);
      ::_exit(127);
    }
    return child;
  }

  void expect_worker_error(std::string const& worker, std::vector<std::uint8_t> const& bytes)
  {
    int sockets[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    auto const child = spawn(worker, sockets[0], sockets[1]);
    ::close(sockets[1]);
    wcls::ipc::write_all(sockets[0], bytes);
    assert(::shutdown(sockets[0], SHUT_WR) == 0);
    auto const response = wcls::ipc::read_message(sockets[0], 1024U * 1024U);
    assert(response.envelope.message_type == wcls::ipc::MessageType::Error);
    assert(!wcls::ipc::decode_error_message(response).empty());
    ::close(sockets[0]);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 2);
  }

  void expect_peer_close_exit(std::string const& worker)
  {
    int sockets[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    auto const child = spawn(worker, sockets[0], sockets[1]);
    ::close(sockets[1]);
    std::uint8_t const partial[3] = {'S', 'B', 'N'};
    assert(::write(sockets[0], partial, sizeof partial) == static_cast<ssize_t>(sizeof partial));
    ::close(sockets[0]);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 2);
  }

} // namespace

int main(int const argc, char const* const* const argv)
{
  assert(argc == 2);
  std::string const worker{argv[1]};
  auto const good = wcls::ipc::encode_reco_frame_payload(payload());

  auto truncated_header = std::vector<std::uint8_t>{good.begin(), good.begin() + 12U};
  expect_worker_error(worker, truncated_header);

  auto truncated_payload = good;
  truncated_payload.pop_back();
  expect_worker_error(worker, truncated_payload);

  auto invalid_magic = good;
  invalid_magic[0] ^= 0xffU;
  expect_worker_error(worker, invalid_magic);

  auto invalid_version = good;
  set_le16(invalid_version, 8U, 99U);
  expect_worker_error(worker, invalid_version);

  auto wrong_type = good;
  set_le16(wrong_type, 10U, static_cast<std::uint16_t>(wcls::ipc::MessageType::Ack));
  expect_worker_error(worker, wrong_type);

  auto absurd_length = good;
  set_le64(absurd_length, 12U, 65U * 1024U * 1024U);
  expect_worker_error(worker, absurd_length);

  auto malformed_vector = good;
  set_le32(malformed_vector, wcls::ipc::kEnvelopeBytes + 28U, 0xffffffffU);
  expect_worker_error(worker, malformed_vector);

  expect_peer_close_exit(worker);
  return 0;
}
