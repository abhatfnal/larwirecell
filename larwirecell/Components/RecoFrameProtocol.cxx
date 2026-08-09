#include "RecoFrameProtocol.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace {

  constexpr std::array<std::uint8_t, 8U> magic{{'S', 'B', 'N', 'D', 'I', 'P', 'C', 0U}};

  class Writer {
  public:
    void bytes(std::uint8_t const* const data, std::size_t const size)
    {
      if (size == 0U) {
        return;
      }
      if (size > std::numeric_limits<std::size_t>::max() - data_.size()) {
        throw wcls::ipc::ProtocolError("wire encoding size overflow");
      }
      data_.insert(data_.end(), data, data + size);
    }

    void u16(std::uint16_t const value)
    {
      data_.push_back(static_cast<std::uint8_t>(value));
      data_.push_back(static_cast<std::uint8_t>(value >> 8U));
    }

    void u32(std::uint32_t const value)
    {
      for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        data_.push_back(static_cast<std::uint8_t>(value >> shift));
      }
    }

    void i32(std::int32_t const value)
    {
      std::uint32_t raw = 0U;
      std::memcpy(&raw, &value, sizeof raw);
      u32(raw);
    }

    void u64(std::uint64_t const value)
    {
      for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        data_.push_back(static_cast<std::uint8_t>(value >> shift));
      }
    }

    void f32(float const value)
    {
      std::uint32_t raw = 0U;
      std::memcpy(&raw, &value, sizeof raw);
      u32(raw);
    }

    void f64(double const value)
    {
      std::uint64_t raw = 0U;
      std::memcpy(&raw, &value, sizeof raw);
      u64(raw);
    }

    void string(std::string const& value)
    {
      if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw wcls::ipc::ProtocolError("string exceeds uint32_t wire length");
      }
      u32(static_cast<std::uint32_t>(value.size()));
      bytes(reinterpret_cast<std::uint8_t const*>(value.data()), value.size());
    }

    std::vector<std::uint8_t> take() { return std::move(data_); }

  private:
    std::vector<std::uint8_t> data_;
  };

  class Reader {
  public:
    Reader(std::uint8_t const* const data, std::size_t const size) : data_{data}, size_{size} {}

    std::size_t remaining() const { return size_ - position_; }

    void require(std::size_t const size, char const* const what) const
    {
      if (size > remaining()) {
        throw wcls::ipc::ProtocolError(std::string{"truncated "} + what);
      }
    }

    std::uint8_t u8(char const* const what)
    {
      require(1U, what);
      return data_[position_++];
    }

    std::uint16_t u16(char const* const what)
    {
      std::uint16_t result = 0U;
      for (unsigned int shift = 0U; shift < 16U; shift += 8U) {
        result |= static_cast<std::uint16_t>(u8(what)) << shift;
      }
      return result;
    }

    std::uint32_t u32(char const* const what)
    {
      std::uint32_t result = 0U;
      for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        result |= static_cast<std::uint32_t>(u8(what)) << shift;
      }
      return result;
    }

    std::int32_t i32(char const* const what)
    {
      auto const raw = u32(what);
      std::int32_t result = 0;
      std::memcpy(&result, &raw, sizeof result);
      return result;
    }

    std::uint64_t u64(char const* const what)
    {
      std::uint64_t result = 0U;
      for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        result |= static_cast<std::uint64_t>(u8(what)) << shift;
      }
      return result;
    }

    float f32(char const* const what)
    {
      auto const raw = u32(what);
      float result = 0.0F;
      std::memcpy(&result, &raw, sizeof result);
      return result;
    }

    double f64(char const* const what)
    {
      auto const raw = u64(what);
      double result = 0.0;
      std::memcpy(&result, &raw, sizeof result);
      return result;
    }

    std::uint32_t count(std::uint32_t const limit,
                        std::size_t const minimum_element_bytes,
                        char const* const what)
    {
      auto const result = u32(what);
      if (result > limit) {
        throw wcls::ipc::ProtocolError(std::string{what} + " exceeds decoder limit");
      }
      if (minimum_element_bytes != 0U &&
          static_cast<std::size_t>(result) > remaining() / minimum_element_bytes) {
        throw wcls::ipc::ProtocolError(std::string{what} + " exceeds remaining message bytes");
      }
      return result;
    }

    std::string string(std::uint32_t const limit, char const* const what)
    {
      auto const size = count(limit, 1U, what);
      require(size, what);
      if (size == 0U) {
        return {};
      }
      std::string result{reinterpret_cast<char const*>(data_ + position_), size};
      position_ += size;
      return result;
    }

    void require_end(char const* const what) const
    {
      if (remaining() != 0U) {
        throw wcls::ipc::ProtocolError(std::string{"trailing bytes after "} + what);
      }
    }

  private:
    std::uint8_t const* data_;
    std::size_t size_;
    std::size_t position_ = 0U;
  };

  bool known_message_type(std::uint16_t const raw)
  {
    using wcls::ipc::MessageType;
    switch (static_cast<MessageType>(raw)) {
    case MessageType::RecoFramePayload:
    case MessageType::TruthArrays:
    case MessageType::RecoArraysApa0:
    case MessageType::RecoArraysApa1:
    case MessageType::Ack:
    case MessageType::Error:
    case MessageType::EndOfStream:
    case MessageType::TestIFrame:
    case MessageType::TestWorkerReport: return true;
    }
    return false;
  }

  wcls::ipc::MessageEnvelope decode_envelope(std::uint8_t const* const bytes,
                                              std::size_t const size,
                                              std::uint64_t const max_payload_bytes)
  {
    if (size != wcls::ipc::kEnvelopeBytes) {
      throw wcls::ipc::ProtocolError("truncated IPC envelope");
    }
    Reader reader{bytes, size};
    for (auto const expected : magic) {
      if (reader.u8("IPC magic") != expected) {
        throw wcls::ipc::ProtocolError("invalid IPC magic");
      }
    }

    wcls::ipc::MessageEnvelope envelope;
    envelope.protocol_version = reader.u16("protocol version");
    if (envelope.protocol_version != wcls::ipc::kProtocolVersion) {
      throw wcls::ipc::ProtocolError("unsupported IPC protocol version");
    }
    auto const raw_type = reader.u16("message type");
    if (!known_message_type(raw_type)) {
      throw wcls::ipc::ProtocolError("unknown IPC message type");
    }
    envelope.message_type = static_cast<wcls::ipc::MessageType>(raw_type);
    envelope.payload_bytes = reader.u64("payload byte count");
    if (envelope.payload_bytes > max_payload_bytes ||
        envelope.payload_bytes > std::numeric_limits<std::size_t>::max()) {
      throw wcls::ipc::ProtocolError("declared IPC payload exceeds decoder limit");
    }
    envelope.identity.run = reader.u32("run");
    envelope.identity.subrun = reader.u32("subrun");
    envelope.identity.event = reader.u32("event");
    reader.require_end("IPC envelope");
    return envelope;
  }

  void add_total(std::uint32_t& total,
                 std::uint32_t const amount,
                 std::uint32_t const limit,
                 char const* const what)
  {
    if (amount > limit - total) {
      throw wcls::ipc::ProtocolError(std::string{"total "} + what + " exceeds decoder limit");
    }
    total += amount;
  }

  std::uint32_t plus_one_limit(std::uint32_t const value, char const* const what)
  {
    if (value == std::numeric_limits<std::uint32_t>::max()) {
      throw wcls::ipc::ProtocolError(std::string{what} + " decoder limit overflows");
    }
    return value + 1U;
  }

  void encode_u32_vector(Writer& writer, std::vector<std::uint32_t> const& values)
  {
    if (values.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw wcls::ipc::ProtocolError("uint32 vector exceeds wire count");
    }
    writer.u32(static_cast<std::uint32_t>(values.size()));
    for (auto const value : values) {
      writer.u32(value);
    }
  }

  void encode_i32_vector(Writer& writer, std::vector<std::int32_t> const& values)
  {
    if (values.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw wcls::ipc::ProtocolError("int32 vector exceeds wire count");
    }
    writer.u32(static_cast<std::uint32_t>(values.size()));
    for (auto const value : values) {
      writer.i32(value);
    }
  }

  std::vector<std::uint32_t> decode_u32_vector(Reader& reader,
                                                std::uint32_t const limit,
                                                char const* const what)
  {
    auto const count = reader.count(limit, sizeof(std::uint32_t), what);
    std::vector<std::uint32_t> result;
    result.reserve(count);
    for (std::uint32_t ind = 0U; ind < count; ++ind) {
      result.push_back(reader.u32(what));
    }
    return result;
  }

  std::vector<std::int32_t> decode_i32_vector(Reader& reader,
                                               std::uint32_t const limit,
                                               char const* const what)
  {
    auto const count = reader.count(limit, sizeof(std::int32_t), what);
    std::vector<std::int32_t> result;
    result.reserve(count);
    for (std::uint32_t ind = 0U; ind < count; ++ind) {
      result.push_back(reader.i32(what));
    }
    return result;
  }

} // namespace

std::vector<std::uint8_t> wcls::ipc::encode_message(MessageEnvelope envelope,
                                                     std::vector<std::uint8_t> const& payload)
{
  envelope.payload_bytes = payload.size();
  Writer writer;
  writer.bytes(magic.data(), magic.size());
  writer.u16(envelope.protocol_version);
  writer.u16(static_cast<std::uint16_t>(envelope.message_type));
  writer.u64(envelope.payload_bytes);
  writer.u32(envelope.identity.run);
  writer.u32(envelope.identity.subrun);
  writer.u32(envelope.identity.event);
  writer.bytes(payload.data(), payload.size());
  return writer.take();
}

wcls::ipc::Message wcls::ipc::decode_message(std::vector<std::uint8_t> const& bytes,
                                              std::uint64_t const max_payload_bytes)
{
  if (bytes.size() < kEnvelopeBytes) {
    throw ProtocolError("truncated IPC envelope");
  }
  auto const envelope = decode_envelope(bytes.data(), kEnvelopeBytes, max_payload_bytes);
  auto const payload_size = static_cast<std::size_t>(envelope.payload_bytes);
  if (payload_size > std::numeric_limits<std::size_t>::max() - kEnvelopeBytes) {
    throw ProtocolError("IPC message size overflows size_t");
  }
  auto const message_size = kEnvelopeBytes + payload_size;
  if (bytes.size() != message_size) {
    throw ProtocolError(bytes.size() < message_size ? "truncated IPC payload" :
                                                      "trailing bytes after IPC payload");
  }
  Message message;
  message.envelope = envelope;
  message.payload.assign(bytes.begin() + kEnvelopeBytes, bytes.end());
  return message;
}

std::vector<std::uint8_t> wcls::ipc::encode_reco_frame_payload(RecoFramePayload const& payload)
{
  validate_reco_frame_payload(payload);
  Writer writer;
  writer.u32(payload.schema_version);
  writer.i32(payload.frame_ident);
  writer.f64(payload.frame_time_value);
  writer.f64(payload.tick_period_ns);
  writer.u32(payload.nticks);

  if (payload.frame_tags.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError("frame tag count exceeds wire count");
  }
  writer.u32(static_cast<std::uint32_t>(payload.frame_tags.size()));
  for (auto const& tag : payload.frame_tags) {
    writer.string(tag);
  }

  if (payload.trace_sets.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError("trace-set count exceeds wire count");
  }
  writer.u32(static_cast<std::uint32_t>(payload.trace_sets.size()));
  for (auto const& trace_set : payload.trace_sets) {
    writer.string(trace_set.trace_tag);
    writer.i32(trace_set.tbin);
    encode_u32_vector(writer, trace_set.channels);
    encode_u32_vector(writer, trace_set.trace_range_offsets);
    encode_u32_vector(writer, trace_set.range_begin_tick);
    encode_u32_vector(writer, trace_set.range_value_offsets);
    if (trace_set.values.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw ProtocolError("float vector exceeds wire count");
    }
    writer.u32(static_cast<std::uint32_t>(trace_set.values.size()));
    for (auto const value : trace_set.values) {
      writer.f32(value);
    }
    if (trace_set.summary.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw ProtocolError("summary vector exceeds wire count");
    }
    writer.u32(static_cast<std::uint32_t>(trace_set.summary.size()));
    for (auto const value : trace_set.summary) {
      writer.f64(value);
    }
  }

  if (payload.masks.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError("mask-set count exceeds wire count");
  }
  writer.u32(static_cast<std::uint32_t>(payload.masks.size()));
  for (auto const& mask_set : payload.masks) {
    writer.string(mask_set.tag);
    encode_i32_vector(writer, mask_set.channel);
    encode_i32_vector(writer, mask_set.begin_tick);
    encode_i32_vector(writer, mask_set.end_tick);
  }

  MessageEnvelope envelope;
  envelope.message_type = MessageType::RecoFramePayload;
  envelope.identity = payload.identity;
  return encode_message(envelope, writer.take());
}

wcls::RecoFramePayload wcls::ipc::decode_reco_frame_payload(Message const& message,
                                                             DecodeLimits const& limits)
{
  if (message.envelope.message_type != MessageType::RecoFramePayload) {
    throw ProtocolError("wrong IPC message type for RecoFramePayload");
  }
  if (message.payload.size() != message.envelope.payload_bytes ||
      message.payload.size() > limits.max_message_payload_bytes) {
    throw ProtocolError("RecoFramePayload message length is invalid");
  }

  Reader reader{message.payload.data(), message.payload.size()};
  RecoFramePayload result;
  result.schema_version = reader.u32("RecoFramePayload schema_version");
  result.identity = message.envelope.identity;
  result.frame_ident = reader.i32("frame_ident");
  result.frame_time_value = reader.f64("frame_time_value");
  result.tick_period_ns = reader.f64("tick_period_ns");
  result.nticks = reader.u32("nticks");

  auto const frame_tag_count = reader.count(limits.max_frame_tags, 4U, "frame tag count");
  result.frame_tags.reserve(frame_tag_count);
  for (std::uint32_t ind = 0U; ind < frame_tag_count; ++ind) {
    result.frame_tags.push_back(reader.string(limits.max_string_bytes, "frame tag"));
  }

  auto const trace_set_count = reader.count(limits.max_trace_sets, 32U, "trace-set count");
  result.trace_sets.reserve(trace_set_count);
  std::uint32_t total_traces = 0U;
  std::uint32_t total_ranges = 0U;
  std::uint32_t total_values = 0U;
  std::uint32_t total_summaries = 0U;
  for (std::uint32_t set_index = 0U; set_index < trace_set_count; ++set_index) {
    SparseTraceSet trace_set;
    trace_set.trace_tag = reader.string(limits.max_string_bytes, "trace tag");
    trace_set.tbin = reader.i32("trace tbin");
    trace_set.channels = decode_u32_vector(reader, limits.max_traces, "channel count");
    add_total(total_traces,
              static_cast<std::uint32_t>(trace_set.channels.size()),
              limits.max_traces,
              "trace count");

    trace_set.trace_range_offsets =
      decode_u32_vector(reader,
                        plus_one_limit(limits.max_traces, "trace count"),
                        "trace-range offset count");
    if (trace_set.trace_range_offsets.size() != trace_set.channels.size() + 1U) {
      throw ProtocolError("trace-range offset count is not ntraces+1");
    }
    trace_set.range_begin_tick =
      decode_u32_vector(reader, limits.max_ranges, "ROI-range count");
    add_total(total_ranges,
              static_cast<std::uint32_t>(trace_set.range_begin_tick.size()),
              limits.max_ranges,
              "ROI-range count");
    trace_set.range_value_offsets =
      decode_u32_vector(reader,
                        plus_one_limit(limits.max_ranges, "ROI-range count"),
                        "range-value offset count");
    if (trace_set.range_value_offsets.size() != trace_set.range_begin_tick.size() + 1U) {
      throw ProtocolError("range-value offset count is not nranges+1");
    }

    auto const value_count = reader.count(limits.max_values, sizeof(float), "sample count");
    add_total(total_values, value_count, limits.max_values, "sample count");
    trace_set.values.reserve(value_count);
    for (std::uint32_t ind = 0U; ind < value_count; ++ind) {
      trace_set.values.push_back(reader.f32("float sample"));
    }

    auto const summary_count =
      reader.count(limits.max_summaries, sizeof(double), "summary count");
    if (summary_count != 0U && summary_count != trace_set.channels.size()) {
      throw ProtocolError("summary count is neither zero nor ntraces");
    }
    add_total(total_summaries, summary_count, limits.max_summaries, "summary count");
    trace_set.summary.reserve(summary_count);
    for (std::uint32_t ind = 0U; ind < summary_count; ++ind) {
      trace_set.summary.push_back(reader.f64("double summary"));
    }
    result.trace_sets.push_back(std::move(trace_set));
  }

  auto const mask_set_count = reader.count(limits.max_mask_sets, 20U, "mask-set count");
  result.masks.reserve(mask_set_count);
  std::uint32_t total_mask_ranges = 0U;
  for (std::uint32_t set_index = 0U; set_index < mask_set_count; ++set_index) {
    TaggedMaskSet mask_set;
    mask_set.tag = reader.string(limits.max_string_bytes, "mask tag");
    mask_set.channel = decode_i32_vector(reader, limits.max_mask_ranges, "mask channel count");
    add_total(total_mask_ranges,
              static_cast<std::uint32_t>(mask_set.channel.size()),
              limits.max_mask_ranges,
              "mask range count");
    mask_set.begin_tick =
      decode_i32_vector(reader, limits.max_mask_ranges, "mask begin count");
    mask_set.end_tick = decode_i32_vector(reader, limits.max_mask_ranges, "mask end count");
    if (mask_set.channel.size() != mask_set.begin_tick.size() ||
        mask_set.channel.size() != mask_set.end_tick.size()) {
      throw ProtocolError("mask vector lengths differ");
    }
    result.masks.push_back(std::move(mask_set));
  }

  reader.require_end("RecoFramePayload");
  validate_reco_frame_payload(result);
  return result;
}

wcls::RecoFramePayload wcls::ipc::decode_reco_frame_payload(
  std::vector<std::uint8_t> const& bytes,
  DecodeLimits const& limits)
{
  return decode_reco_frame_payload(decode_message(bytes, limits.max_message_payload_bytes), limits);
}

void wcls::ipc::write_all(int const fd, std::uint8_t const* data, std::size_t size)
{
  while (size != 0U) {
    auto const written = ::send(fd, data, size, MSG_NOSIGNAL);
    if (written > 0) {
      data += written;
      size -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    std::ostringstream message;
    message << "socket write failed: " << std::strerror(errno);
    throw ProtocolError(message.str());
  }
}

void wcls::ipc::write_all(int const fd, std::vector<std::uint8_t> const& bytes)
{
  write_all(fd, bytes.data(), bytes.size());
}

wcls::ipc::Message wcls::ipc::read_message(int const fd,
                                            std::uint64_t const max_payload_bytes)
{
  std::array<std::uint8_t, kEnvelopeBytes> header{};
  std::size_t received = 0U;
  while (received != header.size()) {
    auto const count = ::read(fd, header.data() + received, header.size() - received);
    if (count > 0) {
      received += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count == 0) {
      throw ProtocolError(received == 0U ? "peer closed before IPC envelope" :
                                           "peer closed during IPC envelope");
    }
    throw ProtocolError(std::string{"socket read failed: "} + std::strerror(errno));
  }

  Message result;
  result.envelope = decode_envelope(header.data(), header.size(), max_payload_bytes);
  result.payload.resize(static_cast<std::size_t>(result.envelope.payload_bytes));
  received = 0U;
  while (received != result.payload.size()) {
    auto const count =
      ::read(fd, result.payload.data() + received, result.payload.size() - received);
    if (count > 0) {
      received += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count == 0) {
      throw ProtocolError("peer closed during IPC payload");
    }
    throw ProtocolError(std::string{"socket read failed: "} + std::strerror(errno));
  }
  return result;
}

std::vector<std::uint8_t> wcls::ipc::encode_error_message(EventRecoIdentity const identity,
                                                           std::string const& error)
{
  Writer writer;
  writer.string(error);
  MessageEnvelope envelope;
  envelope.message_type = MessageType::Error;
  envelope.identity = identity;
  return encode_message(envelope, writer.take());
}

std::string wcls::ipc::decode_error_message(Message const& message,
                                             std::uint32_t const max_string_bytes)
{
  if (message.envelope.message_type != MessageType::Error) {
    throw ProtocolError("wrong IPC message type for error response");
  }
  Reader reader{message.payload.data(), message.payload.size()};
  auto result = reader.string(max_string_bytes, "error string");
  reader.require_end("error message");
  return result;
}
