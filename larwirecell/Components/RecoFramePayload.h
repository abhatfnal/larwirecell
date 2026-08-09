#ifndef LARWIRECELL_COMPONENTS_RECOFRAMEPAYLOAD_H
#define LARWIRECELL_COMPONENTS_RECOFRAMEPAYLOAD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wcls {

  inline constexpr std::uint32_t kRecoFramePayloadSchemaVersion = 1U;

  // Plain, owning event identity.  These types intentionally do not depend on
  // art, ROOT, recob::Wire, or any Wire-Cell ABI.
  struct EventRecoIdentity {
    std::uint32_t run = 0U;
    std::uint32_t subrun = 0U;
    std::uint32_t event = 0U;
  };

  // Sparse traces in collection order.  trace_range_offsets maps each trace
  // to a half-open range of ROI indices.  range_value_offsets maps each ROI to
  // its consecutive stored samples.  Ticks outside the ROIs are exactly zero.
  struct SparseTraceSet {
    std::string trace_tag;
    std::int32_t tbin = 0;
    std::vector<std::uint32_t> channels;
    std::vector<std::uint32_t> trace_range_offsets;
    std::vector<std::uint32_t> range_begin_tick;
    std::vector<std::uint32_t> range_value_offsets;
    std::vector<float> values;
    std::vector<double> summary;
  };

  // Parallel arrays preserve input triplet order and half-open [begin,end)
  // mask semantics.  Repeated channels are allowed and remain ordered.
  struct TaggedMaskSet {
    std::string tag;
    std::vector<std::int32_t> channel;
    std::vector<std::int32_t> begin_tick;
    std::vector<std::int32_t> end_tick;
  };

  struct RecoFramePayload {
    std::uint32_t schema_version = kRecoFramePayloadSchemaVersion;
    EventRecoIdentity identity;
    std::int32_t frame_ident = 0;
    double frame_time_value = 0.0;
    double tick_period_ns = 0.0;
    std::uint32_t nticks = 0U;
    std::vector<std::string> frame_tags;
    std::vector<SparseTraceSet> trace_sets;
    std::vector<TaggedMaskSet> masks;
  };

  // Throws std::invalid_argument or std::length_error before a builder makes
  // any dense trace allocation.
  void validate_reco_frame_payload(RecoFramePayload const& payload);

  // Logical payload bytes: scalar fields, string contents, and vector element
  // storage.  Allocator capacity and C++ object bookkeeping are excluded.
  std::size_t reco_frame_payload_bytes(RecoFramePayload const& payload);

} // namespace wcls

#endif
