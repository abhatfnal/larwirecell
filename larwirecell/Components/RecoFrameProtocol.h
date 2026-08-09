#ifndef LARWIRECELL_COMPONENTS_RECOFRAMEPROTOCOL_H
#define LARWIRECELL_COMPONENTS_RECOFRAMEPROTOCOL_H

#include "RecoArrays.h"
#include "RecoFramePayload.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace wcls::ipc {

  inline constexpr std::uint16_t kProtocolVersion = 1U;
  inline constexpr std::size_t kEnvelopeBytes = 32U;
  inline constexpr std::uint32_t kRecoArraysSchemaVersion = 1U;

  enum class MessageType : std::uint16_t {
    RecoFramePayload = 1U,
    TruthArrays = 2U,      // reserved; no codec in this milestone
    RecoArraysApa0 = 3U,
    RecoArraysApa1 = 4U,
    Ack = 5U,
    Error = 6U,
    EndOfStream = 7U,
    TestIFrame = 0x8001U,
    TestWorkerReport = 0x8002U
  };

  struct MessageEnvelope {
    std::uint16_t protocol_version = kProtocolVersion;
    MessageType message_type = MessageType::RecoFramePayload;
    std::uint64_t payload_bytes = 0U;
    EventRecoIdentity identity;
  };

  struct Message {
    MessageEnvelope envelope;
    std::vector<std::uint8_t> payload;
  };

  struct DecodeLimits {
    std::uint64_t max_message_payload_bytes = 64U * 1024U * 1024U;
    std::uint32_t max_trace_sets = 16U;
    std::uint32_t max_frame_tags = 64U;
    std::uint32_t max_string_bytes = 4096U;
    std::uint32_t max_traces = 100000U;
    std::uint32_t max_ranges = 1000000U;
    std::uint32_t max_values = 10000000U;
    std::uint32_t max_summaries = 100000U;
    std::uint32_t max_mask_sets = 64U;
    std::uint32_t max_mask_ranges = 1000000U;
  };

  struct RecoArraysDecodeLimits {
    std::uint64_t max_message_payload_bytes = 256U * 1024U * 1024U;
    std::uint32_t max_arrays = 6U;
    std::uint32_t max_name_bytes = 64U;
    std::uint32_t max_rank = 2U;
    std::uint64_t max_dimension = 50000000U;
    std::uint64_t max_array_elements = 50000000U;
    std::uint64_t max_total_elements = 60000000U;
    std::uint64_t max_array_bytes = 200U * 1024U * 1024U;
  };

  class ProtocolError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  // All integer fields and raw IEEE-754 bit patterns use explicit
  // little-endian byte order.  No C++ object representation is serialized.
  std::vector<std::uint8_t> encode_message(MessageEnvelope envelope,
                                           std::vector<std::uint8_t> const& payload);
  Message decode_message(std::vector<std::uint8_t> const& bytes,
                         std::uint64_t max_payload_bytes);

  std::vector<std::uint8_t> encode_reco_frame_payload(RecoFramePayload const& payload);
  RecoFramePayload decode_reco_frame_payload(Message const& message,
                                              DecodeLimits const& limits = {});
  RecoFramePayload decode_reco_frame_payload(std::vector<std::uint8_t> const& bytes,
                                              DecodeLimits const& limits = {});

  // RecoArrays payload schema v1:
  //   u32 schema, u16 APA, u16 reserved, u32 array count
  // followed by ordered arrays, each encoded as:
  //   string name, u32 rank, rank*u64 shape, u16 dtype, u16 reserved,
  //   u64 raw byte count, explicit little-endian IEEE-754 data.
  // Dtype code 1 is little-endian float32.  Current scientific arrays must be
  // exactly rank two and follow the six-array APA-specific reconstruction order.
  void validate_reco_arrays(RecoArrays const& arrays, int apa);
  std::vector<std::uint8_t> encode_reco_arrays(EventRecoIdentity identity,
                                               int apa,
                                               RecoArrays const& arrays);
  RecoArrays decode_reco_arrays(Message const& message,
                                int expected_apa,
                                RecoArraysDecodeLimits const& limits = {});
  RecoArrays decode_reco_arrays(std::vector<std::uint8_t> const& bytes,
                                int expected_apa,
                                RecoArraysDecodeLimits const& limits = {});
  std::string fingerprint_reco_array(RecoArray const& array);

  // Socket-fd helpers retry partial operations and EINTR.  EOF or peer closure
  // before the requested byte count is an explicit ProtocolError.
  void write_all(int fd, std::uint8_t const* data, std::size_t size);
  void write_all(int fd, std::vector<std::uint8_t> const& bytes);
  Message read_message(int fd, std::uint64_t max_payload_bytes);

  std::vector<std::uint8_t> encode_error_message(EventRecoIdentity identity,
                                                  std::string const& error);
  std::string decode_error_message(Message const& message,
                                   std::uint32_t max_string_bytes = 4096U);

} // namespace wcls::ipc

#endif
