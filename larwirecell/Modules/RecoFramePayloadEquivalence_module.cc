#include "larwirecell/Components/CookedFrameSource.h"
#include "larwirecell/Components/RecoFrameBuilder.h"
#include "larwirecell/Components/RecoFrameExtractor.h"
#include "larwirecell/Components/RecoFramePayload.h"

#include "art/Framework/Core/EDProducer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "cetlib_except/exception.h"
#include "fhiclcpp/ParameterSet.h"

#include "WireCellIface/IFrame.h"
#include "WireCellUtil/Configuration.h"
#include "WireCellUtil/Units.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

namespace {

  template<typename T>
  auto raw_bits(T const value)
  {
    static_assert(std::is_floating_point_v<T>);
    using UInt = std::conditional_t<sizeof(T) == sizeof(std::uint32_t),
                                    std::uint32_t,
                                    std::uint64_t>;
    UInt result = 0U;
    std::memcpy(&result, &value, sizeof value);
    return result;
  }

  template<typename T>
  std::string hex_bits(T const value)
  {
    auto const raw = raw_bits(value);
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(2 * sizeof value) << raw;
    return output.str();
  }

  [[noreturn]] void mismatch(std::string const& detail)
  {
    throw cet::exception("RecoFramePayloadEquivalence") << detail << '\n';
  }

  class Fingerprint64 {
  public:
    template<typename T>
    void scalar(T const& value)
    {
      static_assert(std::is_trivially_copyable_v<T>);
      bytes(&value, sizeof value);
    }

    void size(std::size_t const value) { scalar(static_cast<std::uint64_t>(value)); }

    void string(std::string const& value)
    {
      size(value.size());
      bytes(value.data(), value.size());
    }

    void bytes(void const* const address, std::size_t const count)
    {
      auto const* current = static_cast<unsigned char const*>(address);
      for (std::size_t ind = 0U; ind < count; ++ind) {
        value_ ^= current[ind];
        value_ *= 1099511628211ULL;
      }
    }

    std::string hex() const
    {
      std::ostringstream output;
      output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << value_;
      return output.str();
    }

  private:
    std::uint64_t value_ = 14695981039346656037ULL;
  };

  std::string frame_fingerprint(WireCell::IFrame const& frame)
  {
    Fingerprint64 fingerprint;
    fingerprint.scalar(frame.ident());
    fingerprint.scalar(frame.time());
    fingerprint.scalar(frame.tick());

    auto const& frame_tags = frame.frame_tags();
    fingerprint.size(frame_tags.size());
    for (auto const& tag : frame_tags) {
      fingerprint.string(tag);
    }

    auto const traces = frame.traces();
    fingerprint.size(traces ? traces->size() : 0U);
    if (traces) {
      for (auto const& trace : *traces) {
        bool const present = static_cast<bool>(trace);
        fingerprint.scalar(present);
        if (!present) {
          continue;
        }
        fingerprint.scalar(trace->channel());
        fingerprint.scalar(trace->tbin());
        auto const& charge = trace->charge();
        fingerprint.size(charge.size());
        fingerprint.bytes(charge.data(), charge.size() * sizeof(float));
      }
    }

    auto const& trace_tags = frame.trace_tags();
    fingerprint.size(trace_tags.size());
    for (auto const& tag : trace_tags) {
      fingerprint.string(tag);
      auto const& indices = frame.tagged_traces(tag);
      fingerprint.size(indices.size());
      for (auto const index : indices) {
        fingerprint.size(index);
      }
      auto const& summary = frame.trace_summary(tag);
      fingerprint.size(summary.size());
      fingerprint.bytes(summary.data(), summary.size() * sizeof(double));
    }

    auto const masks = frame.masks();
    fingerprint.size(masks.size());
    for (auto const& tagged_mask : masks) {
      fingerprint.string(tagged_mask.first);
      fingerprint.size(tagged_mask.second.size());
      for (auto const& channel_mask : tagged_mask.second) {
        fingerprint.scalar(channel_mask.first);
        fingerprint.size(channel_mask.second.size());
        for (auto const& range : channel_mask.second) {
          fingerprint.scalar(range.first);
          fingerprint.scalar(range.second);
        }
      }
    }
    return fingerprint.hex();
  }

  void compare_frames(WireCell::IFrame const& reference, WireCell::IFrame const& candidate)
  {
    if (reference.ident() != candidate.ident()) {
      mismatch("frame ident differs: old=" + std::to_string(reference.ident()) +
               " new=" + std::to_string(candidate.ident()));
    }
    if (raw_bits(reference.time()) != raw_bits(candidate.time())) {
      mismatch("frame time differs: old=" + hex_bits(reference.time()) +
               " new=" + hex_bits(candidate.time()));
    }
    if (raw_bits(reference.tick()) != raw_bits(candidate.tick())) {
      mismatch("frame tick differs: old=" + hex_bits(reference.tick()) +
               " new=" + hex_bits(candidate.tick()));
    }

    auto const& old_frame_tags = reference.frame_tags();
    auto const& new_frame_tags = candidate.frame_tags();
    if (old_frame_tags.size() != new_frame_tags.size()) {
      mismatch("frame tag count differs: old=" + std::to_string(old_frame_tags.size()) +
               " new=" + std::to_string(new_frame_tags.size()));
    }
    for (std::size_t ind = 0U; ind < old_frame_tags.size(); ++ind) {
      if (old_frame_tags[ind] != new_frame_tags[ind]) {
        mismatch("first differing frame tag at index " + std::to_string(ind) + ": old=" +
                 old_frame_tags[ind] + " new=" + new_frame_tags[ind]);
      }
    }

    auto const old_traces = reference.traces();
    auto const new_traces = candidate.traces();
    if (static_cast<bool>(old_traces) != static_cast<bool>(new_traces)) {
      mismatch("trace-vector presence differs");
    }
    if (!old_traces) {
      return;
    }
    if (old_traces->size() != new_traces->size()) {
      mismatch("trace count differs: old=" + std::to_string(old_traces->size()) +
               " new=" + std::to_string(new_traces->size()));
    }
    for (std::size_t trace_index = 0U; trace_index < old_traces->size(); ++trace_index) {
      auto const& old_trace = old_traces->at(trace_index);
      auto const& new_trace = new_traces->at(trace_index);
      if (static_cast<bool>(old_trace) != static_cast<bool>(new_trace)) {
        mismatch("trace presence differs at trace " + std::to_string(trace_index));
      }
      if (!old_trace) {
        continue;
      }
      if (old_trace->channel() != new_trace->channel()) {
        mismatch("channel differs at trace " + std::to_string(trace_index) + ": old=" +
                 std::to_string(old_trace->channel()) + " new=" +
                 std::to_string(new_trace->channel()));
      }
      if (old_trace->tbin() != new_trace->tbin()) {
        mismatch("tbin differs at trace " + std::to_string(trace_index) + ": old=" +
                 std::to_string(old_trace->tbin()) + " new=" +
                 std::to_string(new_trace->tbin()));
      }
      auto const& old_charge = old_trace->charge();
      auto const& new_charge = new_trace->charge();
      if (old_charge.size() != new_charge.size()) {
        mismatch("sample count differs at trace " + std::to_string(trace_index) + ": old=" +
                 std::to_string(old_charge.size()) + " new=" +
                 std::to_string(new_charge.size()));
      }
      for (std::size_t tick = 0U; tick < old_charge.size(); ++tick) {
        if (raw_bits(old_charge[tick]) != raw_bits(new_charge[tick])) {
          mismatch("first differing sample at trace " + std::to_string(trace_index) +
                   ", tick " + std::to_string(tick) + ": old=" +
                   hex_bits(old_charge[tick]) + " new=" + hex_bits(new_charge[tick]));
        }
      }
    }

    auto const& old_trace_tags = reference.trace_tags();
    auto const& new_trace_tags = candidate.trace_tags();
    if (old_trace_tags.size() != new_trace_tags.size()) {
      mismatch("trace-tag count differs: old=" + std::to_string(old_trace_tags.size()) +
               " new=" + std::to_string(new_trace_tags.size()));
    }
    for (std::size_t tag_index = 0U; tag_index < old_trace_tags.size(); ++tag_index) {
      auto const& old_tag = old_trace_tags[tag_index];
      auto const& new_tag = new_trace_tags[tag_index];
      if (old_tag != new_tag) {
        mismatch("first differing trace tag at index " + std::to_string(tag_index) +
                 ": old=" + old_tag + " new=" + new_tag);
      }
      auto const& old_indices = reference.tagged_traces(old_tag);
      auto const& new_indices = candidate.tagged_traces(new_tag);
      if (old_indices.size() != new_indices.size()) {
        mismatch("trace-index count differs for tag " + old_tag + ": old=" +
                 std::to_string(old_indices.size()) + " new=" +
                 std::to_string(new_indices.size()));
      }
      for (std::size_t ind = 0U; ind < old_indices.size(); ++ind) {
        if (old_indices[ind] != new_indices[ind]) {
          mismatch("first differing trace index for tag " + old_tag + " at position " +
                   std::to_string(ind) + ": old=" + std::to_string(old_indices[ind]) +
                   " new=" + std::to_string(new_indices[ind]));
        }
      }
      auto const& old_summary = reference.trace_summary(old_tag);
      auto const& new_summary = candidate.trace_summary(new_tag);
      if (old_summary.size() != new_summary.size()) {
        mismatch("summary count differs for tag " + old_tag + ": old=" +
                 std::to_string(old_summary.size()) + " new=" +
                 std::to_string(new_summary.size()));
      }
      for (std::size_t ind = 0U; ind < old_summary.size(); ++ind) {
        if (raw_bits(old_summary[ind]) != raw_bits(new_summary[ind])) {
          mismatch("first differing summary for tag " + old_tag + " at position " +
                   std::to_string(ind) + ": old=" + hex_bits(old_summary[ind]) +
                   " new=" + hex_bits(new_summary[ind]));
        }
      }
    }

    auto const old_masks = reference.masks();
    auto const new_masks = candidate.masks();
    if (old_masks.size() != new_masks.size()) {
      mismatch("mask-tag count differs: old=" + std::to_string(old_masks.size()) +
               " new=" + std::to_string(new_masks.size()));
    }
    auto old_tag = old_masks.begin();
    auto new_tag = new_masks.begin();
    for (; old_tag != old_masks.end(); ++old_tag, ++new_tag) {
      if (old_tag->first != new_tag->first) {
        mismatch("first differing mask tag: old=" + old_tag->first + " new=" + new_tag->first);
      }
      if (old_tag->second.size() != new_tag->second.size()) {
        mismatch("masked-channel count differs for tag " + old_tag->first + ": old=" +
                 std::to_string(old_tag->second.size()) + " new=" +
                 std::to_string(new_tag->second.size()));
      }
      auto old_channel = old_tag->second.begin();
      auto new_channel = new_tag->second.begin();
      for (; old_channel != old_tag->second.end(); ++old_channel, ++new_channel) {
        if (old_channel->first != new_channel->first) {
          mismatch("first differing mask channel for tag " + old_tag->first + ": old=" +
                   std::to_string(old_channel->first) + " new=" +
                   std::to_string(new_channel->first));
        }
        if (old_channel->second.size() != new_channel->second.size()) {
          mismatch("mask-range count differs for tag " + old_tag->first + ", channel " +
                   std::to_string(old_channel->first));
        }
        for (std::size_t ind = 0U; ind < old_channel->second.size(); ++ind) {
          if (old_channel->second[ind] != new_channel->second[ind]) {
            auto const& old_range = old_channel->second[ind];
            auto const& new_range = new_channel->second[ind];
            mismatch("first differing mask range for tag " + old_tag->first + ", channel " +
                     std::to_string(old_channel->first) + ", position " +
                     std::to_string(ind) + ": old=[" + std::to_string(old_range.first) + "," +
                     std::to_string(old_range.second) + ") new=[" +
                     std::to_string(new_range.first) + "," +
                     std::to_string(new_range.second) + ")");
          }
        }
      }
    }
  }

  WireCell::Configuration historical_configuration()
  {
    WireCell::Configuration config;
    config["art_tag"] = "";
    config["tick"] = 500.0 * WireCell::units::ns;
    config["nticks"] = 3400;
    config["frame_scale"] = 50.0;
    config["summary_scale"] = 50.0;
    config["frame_tags"] = Json::arrayValue;
    config["frame_tags"].append("orig");
    config["recobwire_tags"] = Json::arrayValue;
    config["recobwire_tags"].append("simtpc2d:gauss");
    config["recobwire_tags"].append("simtpc2d:wiener");
    config["trace_tags"] = Json::arrayValue;
    config["trace_tags"].append("gauss");
    config["trace_tags"].append("wiener");
    config["summary_tags"] = Json::arrayValue;
    config["summary_tags"].append("");
    config["summary_tags"].append("simtpc2d:wienersummary");
    config["input_mask_tags"] = Json::arrayValue;
    config["input_mask_tags"].append("simtpc2d:badmasks");
    config["output_mask_tags"] = Json::arrayValue;
    config["output_mask_tags"].append("bad");
    return config;
  }

} // namespace

namespace wcls {

  class RecoFramePayloadEquivalence : public art::EDProducer {
  public:
    explicit RecoFramePayloadEquivalence(fhicl::ParameterSet const& parameters)
      : EDProducer{parameters}
      , expected_run_{parameters.get<std::uint32_t>("expected_run")}
      , expected_subrun_{parameters.get<std::uint32_t>("expected_subrun")}
      , expected_event_{parameters.get<std::uint32_t>("expected_event")}
    {
      reference_source_.configure(historical_configuration());
    }

    void produce(art::Event& event) override
    {
      if (event.run() != expected_run_ || event.subRun() != expected_subrun_ ||
          event.event() != expected_event_) {
        mismatch("wrong golden event: got " + std::to_string(event.run()) + "/" +
                 std::to_string(event.subRun()) + "/" + std::to_string(event.event()) +
                 " expected " + std::to_string(expected_run_) + "/" +
                 std::to_string(expected_subrun_) + "/" + std::to_string(expected_event_));
      }

      reference_source_.visit(event);
      WireCell::IFrame::pointer reference;
      WireCell::IFrame::pointer reference_eos;
      WireCell::IFrame::pointer reference_after_eos;
      bool const reference_frame_ok = reference_source_(reference);
      bool const reference_eos_ok = reference_source_(reference_eos);
      bool const reference_exhausted = reference_source_(reference_after_eos);
      if (!reference_frame_ok || !reference || !reference_eos_ok || reference_eos ||
          reference_exhausted || reference_after_eos) {
        mismatch("historical source contract is not frame, null/EOS, exhausted");
      }

      auto const payload = extract_reco_frame_payload(event);
      RecoFramePayloadSource candidate_source{payload};
      WireCell::IFrame::pointer candidate;
      WireCell::IFrame::pointer candidate_eos;
      WireCell::IFrame::pointer candidate_after_eos;
      bool const candidate_frame_ok = candidate_source(candidate);
      bool const candidate_eos_ok = candidate_source(candidate_eos);
      bool const candidate_exhausted = candidate_source(candidate_after_eos);
      if (!candidate_frame_ok || !candidate || !candidate_eos_ok || candidate_eos ||
          candidate_exhausted || candidate_after_eos) {
        mismatch("candidate source contract is not frame, null/EOS, exhausted");
      }

      compare_frames(*reference, *candidate);
      auto const reference_fingerprint = frame_fingerprint(*reference);
      auto const candidate_fingerprint = frame_fingerprint(*candidate);
      if (reference_fingerprint != candidate_fingerprint) {
        mismatch("canonical fingerprints differ after field comparison: old=" +
                 reference_fingerprint + " new=" + candidate_fingerprint);
      }

      std::size_t total_traces = 0U;
      std::size_t total_ranges = 0U;
      std::size_t total_values = 0U;
      std::size_t total_summaries = 0U;
      for (auto const& trace_set : payload.trace_sets) {
        total_traces += trace_set.channels.size();
        total_ranges += trace_set.range_begin_tick.size();
        total_values += trace_set.values.size();
        total_summaries += trace_set.summary.size();
        std::cout << "RECOFRAME_TRACE_SET tag=" << trace_set.trace_tag
                  << " traces=" << trace_set.channels.size()
                  << " ranges=" << trace_set.range_begin_tick.size()
                  << " stored_samples=" << trace_set.values.size()
                  << " summaries=" << trace_set.summary.size() << '\n';
      }
      std::size_t mask_ranges = 0U;
      std::set<std::pair<std::string, std::int32_t>> masked_channels;
      for (auto const& mask_set : payload.masks) {
        mask_ranges += mask_set.channel.size();
        for (auto const channel : mask_set.channel) {
          masked_channels.emplace(mask_set.tag, channel);
        }
      }

      std::cout << "RECOFRAME_EQUIVALENCE PASS event=" << payload.identity.run << "/"
                << payload.identity.subrun << "/" << payload.identity.event
                << " reference_fingerprint=" << reference_fingerprint
                << " candidate_fingerprint=" << candidate_fingerprint
                << " traces=" << total_traces << " nticks=" << payload.nticks
                << " dense_samples=" << total_traces * payload.nticks
                << " sparse_ranges=" << total_ranges << " stored_samples=" << total_values
                << " summaries=" << total_summaries << " mask_ranges=" << mask_ranges
                << " masked_channels=" << masked_channels.size()
                << " payload_bytes=" << reco_frame_payload_bytes(payload) << std::endl;
    }

  private:
    std::uint32_t expected_run_;
    std::uint32_t expected_subrun_;
    std::uint32_t expected_event_;
    CookedFrameSource reference_source_;
  };

} // namespace wcls

DEFINE_ART_MODULE(wcls::RecoFramePayloadEquivalence)
