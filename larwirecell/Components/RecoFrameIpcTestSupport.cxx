#include "RecoFrameIpcTestSupport.h"

#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace {

  class Writer {
  public:
    void u8(std::uint8_t const value) { data_.push_back(value); }

    void u32(std::uint32_t const value)
    {
      for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        u8(static_cast<std::uint8_t>(value >> shift));
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
        u8(static_cast<std::uint8_t>(value >> shift));
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

    void string64(std::string const& value)
    {
      u64(value.size());
      data_.insert(data_.end(), value.begin(), value.end());
    }

    void string32(std::string const& value)
    {
      if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw wcls::ipc::ProtocolError("worker-report string exceeds uint32_t wire length");
      }
      u32(static_cast<std::uint32_t>(value.size()));
      data_.insert(data_.end(), value.begin(), value.end());
    }

    std::vector<std::uint8_t> take() { return std::move(data_); }

  private:
    std::vector<std::uint8_t> data_;
  };

  class Reader {
  public:
    explicit Reader(std::vector<std::uint8_t> const& data) : data_{data} {}

    std::uint8_t u8(char const* const what)
    {
      if (position_ == data_.size()) {
        throw wcls::ipc::ProtocolError(std::string{"truncated "} + what);
      }
      return data_[position_++];
    }

    std::uint32_t u32(char const* const what)
    {
      std::uint32_t result = 0U;
      for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        result |= static_cast<std::uint32_t>(u8(what)) << shift;
      }
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

    std::string string(std::uint32_t const limit, char const* const what)
    {
      auto const size = u32(what);
      if (size > limit || size > data_.size() - position_) {
        throw wcls::ipc::ProtocolError(std::string{what} + " length is invalid");
      }
      std::string result{data_.begin() + position_, data_.begin() + position_ + size};
      position_ += size;
      return result;
    }

    void require_end() const
    {
      if (position_ != data_.size()) {
        throw wcls::ipc::ProtocolError("trailing worker-report bytes");
      }
    }

  private:
    std::vector<std::uint8_t> const& data_;
    std::size_t position_ = 0U;
  };

} // namespace

std::vector<std::uint8_t> wcls::ipc::test::encode_canonical_iframe(
  WireCell::IFrame const& frame)
{
  Writer writer;
  writer.i32(frame.ident());
  writer.f64(frame.time());
  writer.f64(frame.tick());

  auto const& frame_tags = frame.frame_tags();
  writer.u64(frame_tags.size());
  for (auto const& tag : frame_tags) {
    writer.string64(tag);
  }

  auto const traces = frame.traces();
  writer.u64(traces ? traces->size() : 0U);
  if (traces) {
    for (auto const& trace : *traces) {
      writer.u8(trace ? 1U : 0U);
      if (!trace) {
        continue;
      }
      writer.i32(trace->channel());
      writer.i32(trace->tbin());
      auto const& charge = trace->charge();
      writer.u64(charge.size());
      for (auto const value : charge) {
        writer.f32(value);
      }
    }
  }

  auto const& trace_tags = frame.trace_tags();
  writer.u64(trace_tags.size());
  for (auto const& tag : trace_tags) {
    writer.string64(tag);
    auto const& indices = frame.tagged_traces(tag);
    writer.u64(indices.size());
    for (auto const index : indices) {
      writer.u64(index);
    }
    auto const& summary = frame.trace_summary(tag);
    writer.u64(summary.size());
    for (auto const value : summary) {
      writer.f64(value);
    }
  }

  auto const masks = frame.masks();
  writer.u64(masks.size());
  for (auto const& tagged_mask : masks) {
    writer.string64(tagged_mask.first);
    writer.u64(tagged_mask.second.size());
    for (auto const& channel_mask : tagged_mask.second) {
      writer.i32(channel_mask.first);
      writer.u64(channel_mask.second.size());
      for (auto const& range : channel_mask.second) {
        writer.i32(range.first);
        writer.i32(range.second);
      }
    }
  }
  return writer.take();
}

std::string wcls::ipc::test::fingerprint_bytes(std::vector<std::uint8_t> const& bytes)
{
  std::uint64_t value = 14695981039346656037ULL;
  for (auto const byte : bytes) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  std::ostringstream output;
  output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << value;
  return output.str();
}

std::vector<std::uint8_t> wcls::ipc::test::encode_worker_report(
  EventRecoIdentity const identity,
  WorkerReport const& report)
{
  Writer writer;
  writer.u64(report.decode_nanoseconds);
  writer.u64(report.reencode_nanoseconds);
  writer.u64(report.rebuild_nanoseconds);
  writer.u64(report.frame_encode_nanoseconds);
  if (report.loaded_wirecell_libraries.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError("too many loaded Wire-Cell libraries in worker report");
  }
  writer.u32(static_cast<std::uint32_t>(report.loaded_wirecell_libraries.size()));
  for (auto const& library : report.loaded_wirecell_libraries) {
    writer.string32(library);
  }
  writer.string32(report.frame_fingerprint);

  MessageEnvelope envelope;
  envelope.message_type = MessageType::TestWorkerReport;
  envelope.identity = identity;
  return encode_message(envelope, writer.take());
}

wcls::ipc::test::WorkerReport wcls::ipc::test::decode_worker_report(Message const& message)
{
  if (message.envelope.message_type != MessageType::TestWorkerReport) {
    throw ProtocolError("wrong IPC message type for worker report");
  }
  Reader reader{message.payload};
  WorkerReport result;
  result.decode_nanoseconds = reader.u64("worker decode time");
  result.reencode_nanoseconds = reader.u64("worker re-encode time");
  result.rebuild_nanoseconds = reader.u64("worker rebuild time");
  result.frame_encode_nanoseconds = reader.u64("worker frame-encode time");
  auto const library_count = reader.u32("worker library count");
  if (library_count > 256U) {
    throw ProtocolError("worker library count exceeds limit");
  }
  result.loaded_wirecell_libraries.reserve(library_count);
  for (std::uint32_t ind = 0U; ind < library_count; ++ind) {
    result.loaded_wirecell_libraries.push_back(reader.string(16384U, "worker library path"));
  }
  result.frame_fingerprint = reader.string(128U, "worker frame fingerprint");
  reader.require_end();
  return result;
}

std::vector<std::string> wcls::ipc::test::loaded_wirecell_libraries()
{
  std::ifstream maps{"/proc/self/maps"};
  std::set<std::string> libraries;
  std::string line;
  while (std::getline(maps, line)) {
    auto const path_begin = line.find('/');
    if (path_begin == std::string::npos) {
      continue;
    }
    auto const path = line.substr(path_begin);
    if (path.find("/libWireCell") != std::string::npos &&
        path.find(".so") != std::string::npos) {
      libraries.insert(path);
    }
  }
  return {libraries.begin(), libraries.end()};
}
