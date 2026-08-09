#include "RecoFramePayload.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

  constexpr std::uint32_t max_nticks = 1000000U;
  constexpr std::size_t max_traces = 1000000U;
  constexpr std::size_t max_ranges = 10000000U;
  constexpr std::size_t max_values = 1000000000U;
  constexpr std::size_t max_mask_ranges = 10000000U;
  constexpr std::size_t max_dense_samples = 200000000U;

  template<typename T>
  void checked_add(std::size_t& total, std::size_t const count)
  {
    if (count > (std::numeric_limits<std::size_t>::max() - total) / sizeof(T)) {
      throw std::length_error("RecoFramePayload byte count overflow");
    }
    total += count * sizeof(T);
  }

  void checked_string_add(std::size_t& total, std::string const& value)
  {
    if (value.size() > std::numeric_limits<std::size_t>::max() - total) {
      throw std::length_error("RecoFramePayload string byte count overflow");
    }
    total += value.size();
  }

  void validate_offsets(std::vector<std::uint32_t> const& offsets,
                        std::size_t const expected_size,
                        std::size_t const terminal,
                        std::string const& name)
  {
    if (offsets.size() != expected_size) {
      throw std::invalid_argument(name + " has the wrong size");
    }
    if (offsets.empty() || offsets.front() != 0U) {
      throw std::invalid_argument(name + " must start at zero");
    }
    for (std::size_t ind = 1; ind < offsets.size(); ++ind) {
      if (offsets[ind] < offsets[ind - 1]) {
        throw std::invalid_argument(name + " is not monotonic");
      }
    }
    if (static_cast<std::size_t>(offsets.back()) != terminal) {
      throw std::invalid_argument(name + " has the wrong terminal offset");
    }
  }

} // namespace

void wcls::validate_reco_frame_payload(RecoFramePayload const& payload)
{
  if (payload.schema_version != kRecoFramePayloadSchemaVersion) {
    throw std::invalid_argument("unsupported RecoFramePayload schema_version");
  }
  if (payload.nticks == 0U || payload.nticks > max_nticks) {
    throw std::length_error("RecoFramePayload nticks is zero or exceeds the allocation limit");
  }
  if (!std::isfinite(payload.tick_period_ns) || payload.tick_period_ns <= 0.0) {
    throw std::invalid_argument("RecoFramePayload tick_period_ns must be finite and positive");
  }
  if (!std::isfinite(payload.frame_time_value)) {
    throw std::invalid_argument("RecoFramePayload frame_time_value must be finite");
  }

  std::size_t total_traces = 0U;
  std::size_t total_ranges = 0U;
  std::size_t total_values = 0U;
  for (auto const& trace_set : payload.trace_sets) {
    auto const ntraces = trace_set.channels.size();
    auto const nranges = trace_set.range_begin_tick.size();
    auto const nvalues = trace_set.values.size();

    if (ntraces > max_traces - total_traces) {
      throw std::length_error("RecoFramePayload has too many traces");
    }
    total_traces += ntraces;
    if (nranges > max_ranges - total_ranges) {
      throw std::length_error("RecoFramePayload has too many sparse ranges");
    }
    total_ranges += nranges;
    if (nvalues > max_values - total_values) {
      throw std::length_error("RecoFramePayload has too many sparse values");
    }
    total_values += nvalues;

    if (ntraces > std::numeric_limits<std::uint32_t>::max() ||
        nranges > std::numeric_limits<std::uint32_t>::max() ||
        nvalues > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("RecoFramePayload offsets exceed uint32_t capacity");
    }
    validate_offsets(trace_set.trace_range_offsets,
                     ntraces + 1U,
                     nranges,
                     "trace_range_offsets");
    validate_offsets(trace_set.range_value_offsets,
                     nranges + 1U,
                     nvalues,
                     "range_value_offsets");
    if (!trace_set.summary.empty() && trace_set.summary.size() != ntraces) {
      throw std::invalid_argument("RecoFramePayload summary is not aligned with its traces");
    }
    for (auto const channel : trace_set.channels) {
      if (channel > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("RecoFramePayload channel cannot be represented by ITrace");
      }
    }

    for (std::size_t trace_index = 0U; trace_index < ntraces; ++trace_index) {
      auto const first_range = trace_set.trace_range_offsets[trace_index];
      auto const after_range = trace_set.trace_range_offsets[trace_index + 1U];
      std::uint64_t previous_end = 0U;
      for (std::uint32_t range_index = first_range; range_index < after_range; ++range_index) {
        auto const first_value = trace_set.range_value_offsets[range_index];
        auto const after_value = trace_set.range_value_offsets[range_index + 1U];
        if (after_value == first_value) {
          throw std::invalid_argument("RecoFramePayload contains an empty sparse range");
        }
        auto const begin = static_cast<std::uint64_t>(trace_set.range_begin_tick[range_index]);
        auto const length = static_cast<std::uint64_t>(after_value - first_value);
        auto const end = begin + length;
        if (begin < previous_end) {
          throw std::invalid_argument("RecoFramePayload sparse ranges overlap or are out of order");
        }
        if (end > payload.nticks) {
          throw std::invalid_argument("RecoFramePayload sparse range exceeds nticks");
        }
        previous_end = end;
      }
    }
  }

  if (total_traces != 0U &&
      static_cast<std::size_t>(payload.nticks) >
        std::numeric_limits<std::size_t>::max() / total_traces) {
    throw std::length_error("RecoFramePayload dense sample count overflows size_t");
  }
  if (total_traces * static_cast<std::size_t>(payload.nticks) > max_dense_samples) {
    throw std::length_error("RecoFramePayload dense reconstruction request exceeds the limit");
  }

  std::size_t total_mask_ranges = 0U;
  for (auto const& mask_set : payload.masks) {
    if (mask_set.channel.size() != mask_set.begin_tick.size() ||
        mask_set.channel.size() != mask_set.end_tick.size()) {
      throw std::invalid_argument("RecoFramePayload mask arrays have different lengths");
    }
    if (mask_set.channel.size() > max_mask_ranges - total_mask_ranges) {
      throw std::length_error("RecoFramePayload has too many mask ranges");
    }
    total_mask_ranges += mask_set.channel.size();
    for (std::size_t ind = 0U; ind < mask_set.channel.size(); ++ind) {
      if (mask_set.end_tick[ind] < mask_set.begin_tick[ind]) {
        throw std::invalid_argument("RecoFramePayload mask end_tick is before begin_tick");
      }
    }
  }
}

std::size_t wcls::reco_frame_payload_bytes(RecoFramePayload const& payload)
{
  std::size_t total = 0U;
  checked_add<std::uint32_t>(total, 5U); // schema, identity, nticks
  checked_add<std::int32_t>(total, 1U);  // frame_ident
  checked_add<double>(total, 2U);        // frame time and tick

  for (auto const& tag : payload.frame_tags) {
    checked_string_add(total, tag);
  }
  for (auto const& trace_set : payload.trace_sets) {
    checked_string_add(total, trace_set.trace_tag);
    checked_add<std::int32_t>(total, 1U);
    checked_add<std::uint32_t>(total, trace_set.channels.size());
    checked_add<std::uint32_t>(total, trace_set.trace_range_offsets.size());
    checked_add<std::uint32_t>(total, trace_set.range_begin_tick.size());
    checked_add<std::uint32_t>(total, trace_set.range_value_offsets.size());
    checked_add<float>(total, trace_set.values.size());
    checked_add<double>(total, trace_set.summary.size());
  }
  for (auto const& mask_set : payload.masks) {
    checked_string_add(total, mask_set.tag);
    checked_add<std::int32_t>(total, mask_set.channel.size());
    checked_add<std::int32_t>(total, mask_set.begin_tick.size());
    checked_add<std::int32_t>(total, mask_set.end_tick.size());
  }
  return total;
}
