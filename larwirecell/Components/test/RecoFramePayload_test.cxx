#include "larwirecell/Components/RecoFrameBuilder.h"
#include "larwirecell/Components/RecoFrameExtractor.h"
#include "larwirecell/Components/RecoFramePayload.h"

#include "lardataobj/RecoBase/Wire.h"

#include <algorithm>
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

  std::uint32_t bits(float const value)
  {
    std::uint32_t result = 0U;
    static_assert(sizeof result == sizeof value);
    std::memcpy(&result, &value, sizeof result);
    return result;
  }

  std::uint64_t bits(double const value)
  {
    std::uint64_t result = 0U;
    static_assert(sizeof result == sizeof value);
    std::memcpy(&result, &value, sizeof result);
    return result;
  }

  recob::Wire make_wire(std::size_t const signal_size,
                        raw::ChannelID_t const channel,
                        std::vector<std::pair<std::size_t, std::vector<float>>> const& ranges)
  {
    recob::Wire::RegionsOfInterest_t sparse_signal(signal_size);
    for (auto const& range : ranges) {
      sparse_signal.add_range(range.first, range.second);
    }
    return recob::Wire{std::move(sparse_signal), channel, geo::kU};
  }

  wcls::RecoFramePayload make_payload(wcls::SparseTraceSet trace_set,
                                      std::uint32_t const nticks)
  {
    wcls::RecoFramePayload payload;
    payload.identity = {1U, 2U, 3U};
    payload.frame_ident = 3;
    payload.frame_time_value = 1.25;
    payload.tick_period_ns = 500.0;
    payload.nticks = nticks;
    payload.frame_tags = {"orig"};
    payload.trace_sets.push_back(std::move(trace_set));
    return payload;
  }

  std::vector<float> historical_dense(recob::Wire const& wire,
                                      std::uint32_t const nticks,
                                      double const scale)
  {
    std::vector<float> const signal = wire.Signal();
    std::vector<float> result(nticks, 0.0F);
    auto const samples = std::min<std::size_t>(signal.size(), nticks);
    for (std::size_t tick = 0U; tick < samples; ++tick) {
      result[tick] = scale * signal[tick];
    }
    return result;
  }

  WireCell::IFrame::pointer extract_and_build(std::vector<recob::Wire> const& wires,
                                               std::uint32_t const nticks = 3400U,
                                               std::vector<double> const& summary = {})
  {
    auto trace_set =
      wcls::detail::extract_sparse_trace_set(wires, "test", nticks, 50.0, summary, 50.0);
    return wcls::make_reco_frame(make_payload(std::move(trace_set), nticks));
  }

  void require_wire_equivalence(recob::Wire const& wire, std::uint32_t const nticks = 3400U)
  {
    auto const frame = extract_and_build({wire}, nticks);
    auto const traces = frame->traces();
    assert(traces && traces->size() == 1U);
    assert(traces->front()->channel() == static_cast<int>(wire.Channel()));
    assert(traces->front()->tbin() == 0);
    auto const expected = historical_dense(wire, nticks, 50.0);
    auto const& actual = traces->front()->charge();
    assert(actual.size() == expected.size());
    for (std::size_t tick = 0U; tick < expected.size(); ++tick) {
      assert(bits(actual[tick]) == bits(expected[tick]));
    }
  }

  void test_disjoint_ranges()
  {
    require_wire_equivalence(make_wire(16U, 10U, {{2U, {1.0F, 2.0F}}, {9U, {3.0F}}}), 16U);
  }

  void test_leading_hole()
  {
    auto const wire = make_wire(8U, 11U, {{3U, {2.0F, 4.0F}}});
    auto const frame = extract_and_build({wire}, 8U);
    auto const& charge = frame->traces()->front()->charge();
    assert(bits(charge[0]) == bits(0.0F));
    assert(bits(charge[2]) == bits(0.0F));
    require_wire_equivalence(wire, 8U);
  }

  void test_internal_hole()
  {
    auto const wire = make_wire(8U, 12U, {{0U, {1.0F}}, {5U, {2.0F}}});
    auto const frame = extract_and_build({wire}, 8U);
    assert(bits(frame->traces()->front()->charge()[3]) == bits(0.0F));
    require_wire_equivalence(wire, 8U);
  }

  void test_trailing_hole()
  {
    auto const wire = make_wire(9U, 13U, {{1U, {5.0F, 6.0F}}});
    auto const frame = extract_and_build({wire}, 9U);
    assert(bits(frame->traces()->front()->charge()[8]) == bits(0.0F));
    require_wire_equivalence(wire, 9U);
  }

  void test_range_past_3399()
  {
    auto const wire = make_wire(3427U, 14U, {{3398U, {1.0F, 2.0F, 3.0F, 4.0F}}});
    auto const trace_set =
      wcls::detail::extract_sparse_trace_set({wire}, "test", 3400U, 50.0);
    assert(trace_set.values.size() == 2U);
    require_wire_equivalence(wire);
  }

  void test_short_signal()
  {
    auto const wire = make_wire(7U, 15U, {{1U, {1.0F}}});
    auto const frame = extract_and_build({wire});
    assert(frame->traces()->front()->charge().size() == 3400U);
    require_wire_equivalence(wire);
  }

  void test_exact_signal()
  {
    require_wire_equivalence(make_wire(3400U, 16U, {{3399U, {1.0F}}}));
  }

  void test_long_signal()
  {
    require_wire_equivalence(make_wire(3427U, 17U, {{3401U, {9.0F}}}));
  }

  void test_signed_zero()
  {
    auto const wire = make_wire(3U, 18U, {{1U, {-0.0F}}});
    auto const frame = extract_and_build({wire}, 3U);
    auto const& charge = frame->traces()->front()->charge();
    assert(bits(charge[0]) == bits(0.0F));
    assert(bits(charge[1]) == bits(-0.0F));
    require_wire_equivalence(wire, 3U);
  }

  void test_nan_and_infinity()
  {
    auto const nan = std::numeric_limits<float>::quiet_NaN();
    auto const inf = std::numeric_limits<float>::infinity();
    auto const wire = make_wire(4U, 19U, {{0U, {nan, inf, -inf}}});
    auto const frame = extract_and_build({wire}, 4U);
    auto const expected = historical_dense(wire, 4U, 50.0);
    auto const& actual = frame->traces()->front()->charge();
    assert(bits(actual[0]) == bits(expected[0]));
    assert(bits(actual[1]) == bits(expected[1]));
    assert(bits(actual[2]) == bits(expected[2]));
  }

  void test_empty_summary()
  {
    auto const wire = make_wire(2U, 20U, {{0U, {1.0F}}});
    auto const frame = extract_and_build({wire}, 2U);
    assert(frame->trace_summary("test").empty());
  }

  void test_summary_alignment()
  {
    std::vector<recob::Wire> const wires{
      make_wire(2U, 21U, {{0U, {1.0F}}}), make_wire(2U, 22U, {{1U, {2.0F}}})};
    std::vector<double> const summary{0.125, -0.5};
    auto const frame = extract_and_build(wires, 2U, summary);
    auto const& indices = frame->tagged_traces("test");
    auto const& actual = frame->trace_summary("test");
    assert((indices == WireCell::IFrame::trace_list_t{0U, 1U}));
    assert(actual.size() == 2U);
    assert(bits(actual[0]) == bits(summary[0] * 50.0));
    assert(bits(actual[1]) == bits(summary[1] * 50.0));
  }

  void test_multiple_masks_same_channel()
  {
    wcls::SparseTraceSet empty;
    empty.trace_tag = "test";
    empty.trace_range_offsets = {0U};
    empty.range_value_offsets = {0U};
    auto payload = make_payload(std::move(empty), 10U);
    payload.masks.push_back({"bad", {7, 7, 8}, {1, 4, 2}, {3, 9, 5}});
    auto const masks = wcls::make_reco_frame(payload)->masks();
    auto const& channel_seven = masks.at("bad").at(7);
    assert(channel_seven.size() == 2U);
    assert(channel_seven[0] == WireCell::Waveform::BinRange(1, 3));
    assert(channel_seven[1] == WireCell::Waveform::BinRange(4, 9));
  }

  void test_empty_masks_and_source_contract()
  {
    wcls::SparseTraceSet empty;
    empty.trace_tag = "test";
    empty.trace_range_offsets = {0U};
    empty.range_value_offsets = {0U};
    auto const payload = make_payload(std::move(empty), 10U);
    wcls::RecoFramePayloadSource source{payload};
    WireCell::IFrame::pointer frame;
    assert(source(frame) && frame);
    assert(frame->masks().empty());
    assert(source(frame) && !frame);
    assert(!source(frame) && !frame);
  }

  template<typename Change>
  void require_invalid(wcls::RecoFramePayload payload, Change change)
  {
    change(payload);
    bool rejected = false;
    try {
      wcls::validate_reco_frame_payload(payload);
    }
    catch (std::exception const&) {
      rejected = true;
    }
    assert(rejected);
  }

  void test_schema_validation()
  {
    auto trace_set = wcls::detail::extract_sparse_trace_set(
      {make_wire(4U, 23U, {{1U, {1.0F, 2.0F}}})}, "test", 4U, 50.0, {1.0}, 50.0);
    auto const valid = make_payload(std::move(trace_set), 4U);
    wcls::validate_reco_frame_payload(valid);

    require_invalid(valid, [](auto& value) { value.schema_version = 99U; });
    require_invalid(valid, [](auto& value) { value.nticks = 0U; });
    require_invalid(valid, [](auto& value) { value.nticks = 1000001U; });
    require_invalid(valid,
                    [](auto& value) { value.trace_sets[0].trace_range_offsets[0] = 1U; });
    require_invalid(valid,
                    [](auto& value) { value.trace_sets[0].range_value_offsets.back() = 99U; });
    require_invalid(valid,
                    [](auto& value) { value.trace_sets[0].range_begin_tick[0] = value.nticks; });
    require_invalid(valid, [](auto& value) { value.trace_sets[0].summary.push_back(2.0); });
    require_invalid(valid, [](auto& value) {
      value.masks.push_back({"bad", {1}, {4}, {3}});
    });
    require_invalid(valid, [](auto& value) {
      value.masks.push_back({"bad", {1, 2}, {0}, {1}});
    });

    auto absurd = valid;
    absurd.nticks = 1000000U;
    absurd.trace_sets[0].channels.assign(201U, 1U);
    absurd.trace_sets[0].trace_range_offsets.assign(202U, 0U);
    absurd.trace_sets[0].range_begin_tick.clear();
    absurd.trace_sets[0].range_value_offsets = {0U};
    absurd.trace_sets[0].values.clear();
    absurd.trace_sets[0].summary.clear();
    require_invalid(absurd, [](auto&) {});
  }

} // namespace

int main()
{
  test_disjoint_ranges();
  test_leading_hole();
  test_internal_hole();
  test_trailing_hole();
  test_range_past_3399();
  test_short_signal();
  test_exact_signal();
  test_long_signal();
  test_signed_zero();
  test_nan_and_infinity();
  test_empty_summary();
  test_summary_alignment();
  test_multiple_masks_same_channel();
  test_empty_masks_and_source_contract();
  test_schema_validation();
  return 0;
}
