#include "larwirecell/Components/RecoFramePayload.h"
#include "larwirecell/Components/RecoFrameProtocol.h"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

  template<typename T, typename UInt>
  T from_bits(UInt const raw)
  {
    static_assert(sizeof(T) == sizeof(UInt));
    T result{};
    std::memcpy(&result, &raw, sizeof result);
    return result;
  }

  template<typename T>
  auto bits(T const value)
  {
    using UInt = std::conditional_t<sizeof(T) == sizeof(std::uint32_t),
                                    std::uint32_t,
                                    std::uint64_t>;
    UInt result = 0U;
    std::memcpy(&result, &value, sizeof result);
    return result;
  }

  wcls::RecoFramePayload edge_payload()
  {
    wcls::RecoFramePayload payload;
    payload.identity = {1U, 2236U, 1U};
    payload.frame_ident = 1;
    payload.frame_time_value = -0.0;
    payload.tick_period_ns = 500.0;
    payload.nticks = 5U;
    payload.frame_tags = {"orig", "ordered-second"};

    wcls::SparseTraceSet traces;
    traces.trace_tag = "edge-bits";
    traces.tbin = 0;
    traces.channels = {10U, 11U, 12U, 13U, 14U};
    traces.trace_range_offsets = {0U, 1U, 1U, 1U, 1U, 1U};
    traces.range_begin_tick = {0U};
    traces.range_value_offsets = {0U, 5U};
    traces.values = {0.0F,
                     -0.0F,
                     from_bits<float>(0x7fc12345U),
                     std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity()};
    traces.summary = {0.0,
                      -0.0,
                      from_bits<double>(0x7ff8123456789abcULL),
                      std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity()};
    payload.trace_sets.push_back(std::move(traces));
    payload.masks.push_back({"bad", {7, 7, 8}, {1, 4, 0}, {3, 5, 5}});
    wcls::validate_reco_frame_payload(payload);
    return payload;
  }

  void require_same(wcls::RecoFramePayload const& old_value,
                    wcls::RecoFramePayload const& new_value)
  {
    assert(old_value.schema_version == new_value.schema_version);
    assert(old_value.identity.run == new_value.identity.run);
    assert(old_value.identity.subrun == new_value.identity.subrun);
    assert(old_value.identity.event == new_value.identity.event);
    assert(old_value.frame_ident == new_value.frame_ident);
    assert(bits(old_value.frame_time_value) == bits(new_value.frame_time_value));
    assert(bits(old_value.tick_period_ns) == bits(new_value.tick_period_ns));
    assert(old_value.nticks == new_value.nticks);
    assert(old_value.frame_tags == new_value.frame_tags);
    assert(old_value.trace_sets.size() == new_value.trace_sets.size());
    for (std::size_t set_index = 0U; set_index < old_value.trace_sets.size(); ++set_index) {
      auto const& old_set = old_value.trace_sets[set_index];
      auto const& new_set = new_value.trace_sets[set_index];
      assert(old_set.trace_tag == new_set.trace_tag);
      assert(old_set.tbin == new_set.tbin);
      assert(old_set.channels == new_set.channels);
      assert(old_set.trace_range_offsets == new_set.trace_range_offsets);
      assert(old_set.range_begin_tick == new_set.range_begin_tick);
      assert(old_set.range_value_offsets == new_set.range_value_offsets);
      assert(old_set.values.size() == new_set.values.size());
      for (std::size_t ind = 0U; ind < old_set.values.size(); ++ind) {
        assert(bits(old_set.values[ind]) == bits(new_set.values[ind]));
      }
      assert(old_set.summary.size() == new_set.summary.size());
      for (std::size_t ind = 0U; ind < old_set.summary.size(); ++ind) {
        assert(bits(old_set.summary[ind]) == bits(new_set.summary[ind]));
      }
    }
    assert(old_value.masks.size() == new_value.masks.size());
    for (std::size_t set_index = 0U; set_index < old_value.masks.size(); ++set_index) {
      auto const& old_set = old_value.masks[set_index];
      auto const& new_set = new_value.masks[set_index];
      assert(old_set.tag == new_set.tag);
      assert(old_set.channel == new_set.channel);
      assert(old_set.begin_tick == new_set.begin_tick);
      assert(old_set.end_tick == new_set.end_tick);
    }
  }

  template<typename Action>
  void require_protocol_error(Action action)
  {
    bool rejected = false;
    try {
      action();
    }
    catch (wcls::ipc::ProtocolError const&) {
      rejected = true;
    }
    assert(rejected);
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

  void test_pure_codec()
  {
    auto const original = edge_payload();
    auto const encoded = wcls::ipc::encode_reco_frame_payload(original);
    auto const decoded = wcls::ipc::decode_reco_frame_payload(encoded);
    require_same(original, decoded);
    assert(encoded == wcls::ipc::encode_reco_frame_payload(decoded));
  }

  void test_malformed_messages()
  {
    auto const original = edge_payload();
    auto const good = wcls::ipc::encode_reco_frame_payload(original);

    auto truncated_header = std::vector<std::uint8_t>{good.begin(), good.begin() + 12U};
    require_protocol_error([&] { wcls::ipc::decode_message(truncated_header, 1024U); });

    auto truncated_payload = good;
    truncated_payload.pop_back();
    require_protocol_error(
      [&] { wcls::ipc::decode_message(truncated_payload, 64U * 1024U * 1024U); });

    auto trailing = good;
    trailing.push_back(0U);
    require_protocol_error([&] { wcls::ipc::decode_message(trailing, 64U * 1024U * 1024U); });

    auto invalid_magic = good;
    invalid_magic[0] ^= 0xffU;
    require_protocol_error(
      [&] { wcls::ipc::decode_message(invalid_magic, 64U * 1024U * 1024U); });

    auto invalid_version = good;
    set_le16(invalid_version, 8U, 99U);
    require_protocol_error(
      [&] { wcls::ipc::decode_message(invalid_version, 64U * 1024U * 1024U); });

    auto wrong_type = good;
    set_le16(wrong_type, 10U, static_cast<std::uint16_t>(wcls::ipc::MessageType::Ack));
    require_protocol_error([&] { wcls::ipc::decode_reco_frame_payload(wrong_type); });

    auto absurd_length = good;
    set_le64(absurd_length, 12U, 65U * 1024U * 1024U);
    require_protocol_error(
      [&] { wcls::ipc::decode_message(absurd_length, 64U * 1024U * 1024U); });

    auto absurd_vector = good;
    // Envelope (32) plus fixed payload scalars (28) reaches frame_tag_count.
    set_le32(absurd_vector, wcls::ipc::kEnvelopeBytes + 28U, 0xffffffffU);
    require_protocol_error([&] { wcls::ipc::decode_reco_frame_payload(absurd_vector); });

    auto malformed_offset = original;
    malformed_offset.frame_tags.clear();
    malformed_offset.trace_sets[0].trace_tag = "t";
    auto malformed_offset_bytes = wcls::ipc::encode_reco_frame_payload(malformed_offset);
    // Body: fixed 28, frame-tag count 4, trace-set count 4, string(4+1),
    // tbin 4, channels count+five values (24), then trace-offset count.
    auto const trace_offset_count = wcls::ipc::kEnvelopeBytes + 36U + 5U + 4U + 24U;
    set_le32(malformed_offset_bytes, trace_offset_count, 0U);
    require_protocol_error(
      [&] { wcls::ipc::decode_reco_frame_payload(malformed_offset_bytes); });
  }

  void test_socketpair_round_trip()
  {
    auto payload = edge_payload();
    auto& trace_set = payload.trace_sets.front();
    trace_set.channels = {10U};
    trace_set.trace_range_offsets = {0U, 1U};
    trace_set.range_begin_tick = {0U};
    trace_set.values.assign(100000U, 1.25F);
    trace_set.values[1] = -0.0F;
    trace_set.range_value_offsets = {0U, 100000U};
    trace_set.summary = {from_bits<double>(0x7ff8123456789abcULL)};
    payload.nticks = 100000U;
    wcls::validate_reco_frame_payload(payload);
    auto const encoded = wcls::ipc::encode_reco_frame_payload(payload);

    int sockets[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    int const small_buffer = 1024;
    assert(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof small_buffer) == 0);
    auto const child = ::fork();
    assert(child >= 0);
    if (child == 0) {
      ::close(sockets[0]);
      try {
        auto const message = wcls::ipc::read_message(sockets[1], 64U * 1024U * 1024U);
        auto const decoded = wcls::ipc::decode_reco_frame_payload(message);
        wcls::ipc::write_all(sockets[1], wcls::ipc::encode_reco_frame_payload(decoded));
        ::close(sockets[1]);
        ::_exit(0);
      }
      catch (...) {
        ::_exit(2);
      }
    }

    ::close(sockets[1]);
    wcls::ipc::write_all(sockets[0], encoded);
    auto const returned_message = wcls::ipc::read_message(sockets[0], 64U * 1024U * 1024U);
    auto const returned_payload = wcls::ipc::decode_reco_frame_payload(returned_message);
    ::close(sockets[0]);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    require_same(payload, returned_payload);
    assert(encoded == wcls::ipc::encode_reco_frame_payload(returned_payload));
  }

  void test_peer_closure()
  {
    int sockets[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    std::uint8_t const partial_header[3] = {'S', 'B', 'N'};
    assert(::write(sockets[1], partial_header, sizeof partial_header) ==
           static_cast<ssize_t>(sizeof partial_header));
    ::close(sockets[1]);
    require_protocol_error([&] { wcls::ipc::read_message(sockets[0], 1024U); });
    ::close(sockets[0]);

    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    ::close(sockets[1]);
    require_protocol_error([&] { wcls::ipc::read_message(sockets[0], 1024U); });
    ::close(sockets[0]);
  }

} // namespace

int main()
{
  test_pure_codec();
  test_malformed_messages();
  test_socketpair_round_trip();
  test_peer_closure();
  return 0;
}
