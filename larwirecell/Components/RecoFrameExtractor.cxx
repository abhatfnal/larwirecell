#include "RecoFrameExtractor.h"

#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Principal/Run.h"
#include "canvas/Utilities/InputTag.h"
#include "lardataobj/RecoBase/Wire.h"

#include "TTimeStamp.h"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace {

  double historical_time_difference(art::Timestamp const& begin, art::Timestamp const& event)
  {
    TTimeStamp begin_timestamp(begin.timeHigh(), begin.timeLow());
    TTimeStamp event_timestamp(event.timeHigh(), event.timeLow());
    return event_timestamp.AsDouble() - begin_timestamp.AsDouble();
  }

  std::uint32_t to_u32(std::size_t const value, char const* const what)
  {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error(std::string{what} + " exceeds uint32_t capacity");
    }
    return static_cast<std::uint32_t>(value);
  }

} // namespace

wcls::SparseTraceSet wcls::detail::extract_sparse_trace_set(
  std::vector<recob::Wire> const& wires,
  std::string const& trace_tag,
  std::uint32_t const nticks,
  double const frame_scale,
  std::vector<double> const& summary,
  double const summary_scale)
{
  SparseTraceSet result;
  result.trace_tag = trace_tag;
  result.tbin = 0;
  result.channels.reserve(wires.size());
  result.trace_range_offsets.reserve(wires.size() + 1U);
  result.trace_range_offsets.push_back(0U);
  result.range_value_offsets.push_back(0U);

  for (auto const& wire : wires) {
    auto const channel = wire.Channel();
    if (channel > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("recob::Wire channel exceeds uint32_t capacity");
    }
    result.channels.push_back(static_cast<std::uint32_t>(channel));

    auto const& sparse_signal = wire.SignalROI();
    auto const signal_ticks = std::min<std::size_t>(sparse_signal.size(), nticks);
    for (auto const& range : sparse_signal.get_ranges()) {
      auto const range_begin = range.begin_index();
      if (range_begin >= signal_ticks) {
        break;
      }
      auto const range_end = std::min<std::size_t>(range.end_index(), signal_ticks);
      auto const sample_count = range_end - range_begin;
      if (sample_count == 0U) {
        continue;
      }

      result.range_begin_tick.push_back(to_u32(range_begin, "ROI begin tick"));
      auto sample = range.begin();
      for (std::size_t ind = 0U; ind < sample_count; ++ind, ++sample) {
        // This deliberately preserves CookedFrameSource's double multiply
        // followed by conversion to float on assignment.
        float const scaled_sample = frame_scale * *sample;
        result.values.push_back(scaled_sample);
      }
      result.range_value_offsets.push_back(to_u32(result.values.size(), "sparse value count"));
    }
    result.trace_range_offsets.push_back(
      to_u32(result.range_begin_tick.size(), "sparse range count"));
  }

  result.summary.reserve(summary.size());
  for (auto const value : summary) {
    result.summary.push_back(value * summary_scale);
  }
  return result;
}

wcls::RecoFramePayload wcls::extract_reco_frame_payload(art::Event const& event,
                                                         RecoFrameExtractConfig const& config)
{
  if (config.recobwire_tags.size() != config.trace_tags.size() ||
      config.recobwire_tags.size() != config.summary_tags.size()) {
    throw std::invalid_argument("RecoFrameExtractConfig wire, trace, and summary tags are misaligned");
  }
  if (config.input_mask_tags.size() != config.output_mask_tags.size()) {
    throw std::invalid_argument("RecoFrameExtractConfig input and output mask tags are misaligned");
  }
  if (event.event() > static_cast<decltype(event.event())>(
                        std::numeric_limits<std::int32_t>::max())) {
    throw std::length_error("event number cannot be represented by frame_ident");
  }

  RecoFramePayload payload;
  payload.identity.run = static_cast<std::uint32_t>(event.run());
  payload.identity.subrun = static_cast<std::uint32_t>(event.subRun());
  payload.identity.event = static_cast<std::uint32_t>(event.event());
  payload.frame_ident = static_cast<std::int32_t>(event.event());
  payload.frame_time_value =
    historical_time_difference(event.getRun().beginTime(), event.time());
  payload.tick_period_ns = config.tick_period_ns;
  payload.nticks = config.nticks;
  payload.frame_tags = config.frame_tags;
  payload.trace_sets.reserve(config.recobwire_tags.size());

  for (std::size_t ind = 0U; ind < config.recobwire_tags.size(); ++ind) {
    art::Handle<std::vector<recob::Wire>> wire_handle;
    art::InputTag const wire_tag{config.recobwire_tags[ind]};
    if (!event.getByLabel(wire_tag, wire_handle)) {
      throw std::runtime_error("failed to get vector<recob::Wire>: " +
                               config.recobwire_tags[ind]);
    }

    std::vector<double> empty_summary;
    std::vector<double> const* summary = &empty_summary;
    art::Handle<std::vector<double>> summary_handle;
    if (!config.summary_tags[ind].empty()) {
      art::InputTag const summary_tag{config.summary_tags[ind]};
      if (!event.getByLabel(summary_tag, summary_handle)) {
        throw std::runtime_error("failed to get vector<double>: " + config.summary_tags[ind]);
      }
      summary = summary_handle.product();
    }

    payload.trace_sets.push_back(detail::extract_sparse_trace_set(*wire_handle,
                                                                   config.trace_tags[ind],
                                                                   config.nticks,
                                                                   config.frame_scale,
                                                                   *summary,
                                                                   config.summary_scale));
  }

  std::map<std::string, art::Handle<std::vector<int>>> mask_handles;
  std::map<std::string, std::string> output_mask_tags;
  for (std::size_t ind = 0U; ind < config.input_mask_tags.size(); ++ind) {
    art::InputTag const input_tag{config.input_mask_tags[ind]};
    if (!event.getByLabel(input_tag, mask_handles[config.input_mask_tags[ind]])) {
      throw std::runtime_error("failed to get vector<int>: " + config.input_mask_tags[ind]);
    }
    output_mask_tags[config.input_mask_tags[ind]] = config.output_mask_tags[ind];
  }

  for (auto const& entry : mask_handles) {
    TaggedMaskSet mask_set;
    mask_set.tag = output_mask_tags.at(entry.first);
    auto const& values = *entry.second;
    auto const range_count = values.size() / 3U;
    mask_set.channel.reserve(range_count);
    mask_set.begin_tick.reserve(range_count);
    mask_set.end_tick.reserve(range_count);
    for (std::size_t ind = 0U; ind < range_count; ++ind) {
      mask_set.channel.push_back(static_cast<std::int32_t>(values[3U * ind]));
      mask_set.begin_tick.push_back(static_cast<std::int32_t>(values[3U * ind + 1U]));
      mask_set.end_tick.push_back(static_cast<std::int32_t>(values[3U * ind + 2U]));
    }
    payload.masks.push_back(std::move(mask_set));
  }

  validate_reco_frame_payload(payload);
  return payload;
}
