#include "RecoFrameBuilder.h"

#include "WireCellAux/SimpleFrame.h"
#include "WireCellAux/SimpleTrace.h"
#include "WireCellUtil/Units.h"
#include "WireCellUtil/Waveform.h"

#include <limits>
#include <memory>
#include <stdexcept>

WireCell::IFrame::pointer wcls::make_reco_frame(RecoFramePayload const& payload)
{
  validate_reco_frame_payload(payload);

  auto traces = std::make_unique<WireCell::ITrace::vector>();
  std::size_t trace_count = 0U;
  for (auto const& trace_set : payload.trace_sets) {
    trace_count += trace_set.channels.size();
  }
  traces->reserve(trace_count);

  WireCell::Waveform::ChannelMaskMap channel_masks;
  for (auto const& mask_set : payload.masks) {
    auto& tagged_masks = channel_masks[mask_set.tag];
    for (std::size_t ind = 0U; ind < mask_set.channel.size(); ++ind) {
      tagged_masks[mask_set.channel[ind]].emplace_back(mask_set.begin_tick[ind],
                                                       mask_set.end_tick[ind]);
    }
  }

  std::vector<WireCell::IFrame::trace_list_t> trace_set_indices;
  trace_set_indices.reserve(payload.trace_sets.size());
  for (auto const& trace_set : payload.trace_sets) {
    WireCell::IFrame::trace_list_t indices;
    indices.reserve(trace_set.channels.size());
    for (std::size_t trace_index = 0U; trace_index < trace_set.channels.size(); ++trace_index) {
      auto const channel = trace_set.channels[trace_index];
      if (channel > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("RecoFramePayload channel cannot be represented by ITrace");
      }

      auto trace = std::make_shared<WireCell::Aux::SimpleTrace>(
        static_cast<int>(channel), trace_set.tbin, payload.nticks);
      auto& charge = trace->charge();
      auto const first_range = trace_set.trace_range_offsets[trace_index];
      auto const after_range = trace_set.trace_range_offsets[trace_index + 1U];
      for (std::uint32_t range_index = first_range; range_index < after_range; ++range_index) {
        auto const begin_tick = trace_set.range_begin_tick[range_index];
        auto const first_value = trace_set.range_value_offsets[range_index];
        auto const after_value = trace_set.range_value_offsets[range_index + 1U];
        for (std::uint32_t value_index = first_value; value_index < after_value; ++value_index) {
          charge[begin_tick + value_index - first_value] = trace_set.values[value_index];
        }
      }
      indices.push_back(traces->size());
      traces->push_back(trace);
    }
    trace_set_indices.push_back(std::move(indices));
  }

  auto const shared_traces = WireCell::ITrace::shared_vector{traces.release()};
  auto frame = std::make_shared<WireCell::Aux::SimpleFrame>(
    payload.frame_ident,
    payload.frame_time_value,
    shared_traces,
    payload.tick_period_ns * WireCell::units::ns,
    channel_masks);

  for (auto const& frame_tag : payload.frame_tags) {
    frame->tag_frame(frame_tag);
  }

  for (std::size_t ind = 0U; ind < payload.trace_sets.size(); ++ind) {
    auto const& trace_set = payload.trace_sets[ind];
    if (trace_set.trace_tag.empty()) {
      continue;
    }
    if (trace_set.summary.empty()) {
      frame->tag_traces(trace_set.trace_tag, trace_set_indices[ind]);
    }
    else {
      frame->tag_traces(trace_set.trace_tag, trace_set_indices[ind], trace_set.summary);
    }
  }
  return frame;
}

wcls::RecoFramePayloadSource::RecoFramePayloadSource(RecoFramePayload const& payload)
{
  frames_.push_back(make_reco_frame(payload));
  frames_.push_back(nullptr);
}

bool wcls::RecoFramePayloadSource::operator()(WireCell::IFrame::pointer& frame)
{
  frame = nullptr;
  if (frames_.empty()) {
    return false;
  }
  frame = frames_.front();
  frames_.pop_front();
  return true;
}
