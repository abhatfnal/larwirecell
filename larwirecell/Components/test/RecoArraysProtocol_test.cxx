#include "larwirecell/Components/RecoArrays.h"
#include "larwirecell/Components/RecoFrameProtocol.h"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

  std::uint32_t bits(float const value)
  {
    std::uint32_t result = 0U;
    std::memcpy(&result, &value, sizeof result);
    return result;
  }

  float from_bits(std::uint32_t const value)
  {
    float result = 0.0F;
    std::memcpy(&result, &value, sizeof result);
    return result;
  }

  wcls::RecoArrays fixture(int const apa)
  {
    auto const family = std::string{"ctpc_f"} + std::to_string(apa);
    wcls::RecoArrays result;
    result.arrays = {
      {family + "p0", 1U, 2U, {0.0F, -0.0F}},
      {family + "p1", 1U, 2U, {from_bits(0x7fc12345U), 1.25F}},
      {family + "p2", 1U, 2U, {std::numeric_limits<float>::infinity(), -2.5F}},
      {"blobs", 1U, 2U, {3.0F, 4.0F}},
      {"points", 1U, 2U, {5.0F, 6.0F}},
      {"ppedges", 1U, 2U, {7.0F, 8.0F}},
    };
    return result;
  }

  void require_same(wcls::RecoArrays const& expected, wcls::RecoArrays const& actual)
  {
    assert(expected.arrays.size() == actual.arrays.size());
    for (std::size_t array_index = 0U; array_index < expected.arrays.size(); ++array_index) {
      auto const& lhs = expected.arrays[array_index];
      auto const& rhs = actual.arrays[array_index];
      assert(lhs.name == rhs.name);
      assert(lhs.rows == rhs.rows);
      assert(lhs.columns == rhs.columns);
      assert(lhs.values.size() == rhs.values.size());
      for (std::size_t value_index = 0U; value_index < lhs.values.size(); ++value_index) {
        assert(bits(lhs.values[value_index]) == bits(rhs.values[value_index]));
      }
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

  void test_round_trip()
  {
    wcls::EventRecoIdentity const identity{1U, 2236U, 1U};
    for (int apa = 0; apa != 2; ++apa) {
      auto const original = fixture(apa);
      auto const encoded = wcls::ipc::encode_reco_arrays(identity, apa, original);
      auto const decoded = wcls::ipc::decode_reco_arrays(encoded, apa);
      require_same(original, decoded);
      assert(encoded == wcls::ipc::encode_reco_arrays(identity, apa, decoded));
      for (auto const& array : original.arrays) {
        assert(wcls::ipc::fingerprint_reco_array(array).rfind("fnv1a64:", 0U) == 0U);
      }
    }
  }

  void test_malformed()
  {
    wcls::EventRecoIdentity const identity{1U, 2236U, 1U};
    auto const original = fixture(0);
    auto const good = wcls::ipc::encode_reco_arrays(identity, 0, original);

    auto bad_schema = good;
    set_le32(bad_schema, wcls::ipc::kEnvelopeBytes, 99U);
    require_protocol_error([&] { wcls::ipc::decode_reco_arrays(bad_schema, 0); });

    auto wrong_type = good;
    set_le16(wrong_type,
             10U,
             static_cast<std::uint16_t>(wcls::ipc::MessageType::RecoArraysApa1));
    require_protocol_error([&] { wcls::ipc::decode_reco_arrays(wrong_type, 0); });

    auto wrong_apa = good;
    set_le16(wrong_apa, wcls::ipc::kEnvelopeBytes + 4U, 1U);
    require_protocol_error([&] { wcls::ipc::decode_reco_arrays(wrong_apa, 0); });

    auto invalid_count = good;
    set_le32(invalid_count, wcls::ipc::kEnvelopeBytes + 8U, 7U);
    require_protocol_error([&] { wcls::ipc::decode_reco_arrays(invalid_count, 0); });

    // First array: fixed payload prefix 12, name length 4, name 9, then rank.
    auto invalid_rank = good;
    auto const first_rank = wcls::ipc::kEnvelopeBytes + 12U + 4U + 9U;
    set_le32(invalid_rank, first_rank, 1U);
    require_protocol_error([&] { wcls::ipc::decode_reco_arrays(invalid_rank, 0); });

    // rank, two uint64 dimensions, uint16 dtype, uint16 reserved, uint64 nbytes.
    auto impossible_dimensions = good;
    set_le64(impossible_dimensions, first_rank + 4U, std::numeric_limits<std::uint64_t>::max());
    require_protocol_error(
      [&] { wcls::ipc::decode_reco_arrays(impossible_dimensions, 0); });

    auto wrong_dtype = good;
    set_le16(wrong_dtype, first_rank + 4U + 16U, 99U);
    require_protocol_error([&] { wcls::ipc::decode_reco_arrays(wrong_dtype, 0); });

    auto wrong_nbytes = good;
    set_le64(wrong_nbytes, first_rank + 4U + 16U + 4U, 7U);
    require_protocol_error([&] { wcls::ipc::decode_reco_arrays(wrong_nbytes, 0); });

    auto truncated = good;
    truncated.pop_back();
    require_protocol_error([&] { wcls::ipc::decode_reco_arrays(truncated, 0); });

    auto trailing = good;
    trailing.push_back(0U);
    require_protocol_error([&] { wcls::ipc::decode_reco_arrays(trailing, 0); });
  }

} // namespace

int main()
{
  test_round_trip();
  test_malformed();
  return 0;
}
