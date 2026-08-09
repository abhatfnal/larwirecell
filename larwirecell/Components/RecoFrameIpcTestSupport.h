#ifndef LARWIRECELL_COMPONENTS_RECOFRAMEIPCTESTSUPPORT_H
#define LARWIRECELL_COMPONENTS_RECOFRAMEIPCTESTSUPPORT_H

#include "RecoFramePayload.h"
#include "RecoFrameProtocol.h"

#include "WireCellIface/IFrame.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wcls::ipc::test {

  inline constexpr std::uint64_t kMaxCanonicalIFrameBytes = 512U * 1024U * 1024U;

  struct WorkerReport {
    std::uint64_t decode_nanoseconds = 0U;
    std::uint64_t reencode_nanoseconds = 0U;
    std::uint64_t rebuild_nanoseconds = 0U;
    std::uint64_t frame_encode_nanoseconds = 0U;
    std::vector<std::string> loaded_wirecell_libraries;
    std::string frame_fingerprint;
  };

  // Test-only canonical representation of every downstream-observable IFrame
  // field.  Its field order matches the accepted FNV-1a frame fingerprint.
  std::vector<std::uint8_t> encode_canonical_iframe(WireCell::IFrame const& frame);
  std::string fingerprint_bytes(std::vector<std::uint8_t> const& bytes);

  std::vector<std::uint8_t> encode_worker_report(EventRecoIdentity identity,
                                                 WorkerReport const& report);
  WorkerReport decode_worker_report(Message const& message);

  std::vector<std::string> loaded_wirecell_libraries();

} // namespace wcls::ipc::test

#endif
