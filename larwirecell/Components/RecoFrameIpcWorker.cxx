#include "RecoFrameBuilder.h"
#include "RecoFrameIpcTestSupport.h"
#include "RecoFrameProtocol.h"

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <unistd.h>

namespace {

  using Clock = std::chrono::steady_clock;

  std::uint64_t nanoseconds(Clock::time_point const begin, Clock::time_point const end)
  {
    return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
  }

  int parse_fd(int const argc, char const* const* const argv)
  {
    if (argc != 3 || std::string{argv[1]} != "--ipc-fd") {
      throw wcls::ipc::ProtocolError("usage: reco_frame_ipc_worker --ipc-fd N");
    }
    errno = 0;
    char* end = nullptr;
    auto const parsed = std::strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || parsed < 0 ||
        parsed > std::numeric_limits<int>::max()) {
      throw wcls::ipc::ProtocolError("invalid --ipc-fd value");
    }
    return static_cast<int>(parsed);
  }

} // namespace

int main(int const argc, char const* const* const argv)
{
  int fd = -1;
  wcls::EventRecoIdentity identity;
  try {
    fd = parse_fd(argc, argv);
    auto const incoming = wcls::ipc::read_message(fd, wcls::ipc::DecodeLimits{}.max_message_payload_bytes);
    identity = incoming.envelope.identity;

    auto begin = Clock::now();
    auto const payload = wcls::ipc::decode_reco_frame_payload(incoming);
    auto end = Clock::now();
    wcls::ipc::test::WorkerReport report;
    report.decode_nanoseconds = nanoseconds(begin, end);

    begin = Clock::now();
    auto const echo = wcls::ipc::encode_reco_frame_payload(payload);
    end = Clock::now();
    report.reencode_nanoseconds = nanoseconds(begin, end);
    // Return the canonical payload immediately so the parent can measure the
    // payload-only round trip independently of dense frame reconstruction.
    wcls::ipc::write_all(fd, echo);

    begin = Clock::now();
    wcls::RecoFramePayloadSource source{payload};
    WireCell::IFrame::pointer frame;
    WireCell::IFrame::pointer eos;
    WireCell::IFrame::pointer exhausted;
    bool const frame_ok = source(frame);
    bool const eos_ok = source(eos);
    bool const after_eos = source(exhausted);
    if (!frame_ok || !frame || !eos_ok || eos || after_eos || exhausted) {
      throw wcls::ipc::ProtocolError("worker source contract is not frame, null/EOS, exhausted");
    }
    end = Clock::now();
    report.rebuild_nanoseconds = nanoseconds(begin, end);

    begin = Clock::now();
    auto const canonical_frame = wcls::ipc::test::encode_canonical_iframe(*frame);
    end = Clock::now();
    report.frame_encode_nanoseconds = nanoseconds(begin, end);
    report.frame_fingerprint = wcls::ipc::test::fingerprint_bytes(canonical_frame);
    report.loaded_wirecell_libraries = wcls::ipc::test::loaded_wirecell_libraries();

    wcls::ipc::MessageEnvelope frame_envelope;
    frame_envelope.message_type = wcls::ipc::MessageType::TestIFrame;
    frame_envelope.identity = payload.identity;
    auto const encoded_frame = wcls::ipc::encode_message(frame_envelope, canonical_frame);
    auto const encoded_report = wcls::ipc::test::encode_worker_report(payload.identity, report);

    wcls::ipc::write_all(fd, encoded_report);
    wcls::ipc::write_all(fd, encoded_frame);
    ::close(fd);
    return 0;
  }
  catch (std::exception const& error) {
    if (fd >= 0) {
      try {
        wcls::ipc::write_all(fd, wcls::ipc::encode_error_message(identity, error.what()));
      }
      catch (...) {
      }
      ::close(fd);
    }
    std::cerr << "reco_frame_ipc_worker: " << error.what() << '\n';
    return 2;
  }
}
