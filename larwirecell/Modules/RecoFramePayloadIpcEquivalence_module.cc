#include "larwirecell/Components/CookedFrameSource.h"
#include "larwirecell/Components/RecoFrameExtractor.h"
#include "larwirecell/Components/RecoFrameIpcTestSupport.h"
#include "larwirecell/Components/RecoFramePayload.h"
#include "larwirecell/Components/RecoFrameProtocol.h"

#include "art/Framework/Core/EDProducer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "cetlib_except/exception.h"
#include "fhiclcpp/ParameterSet.h"

#include "WireCellIface/IFrame.h"
#include "WireCellUtil/Configuration.h"
#include "WireCellUtil/Units.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace {

  using Clock = std::chrono::steady_clock;

  [[noreturn]] void fail(std::string const& detail)
  {
    throw cet::exception("RecoFramePayloadIpcEquivalence") << detail << '\n';
  }

  std::uint64_t nanoseconds(Clock::time_point const begin, Clock::time_point const end)
  {
    return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
  }

  std::string milliseconds(std::uint64_t const value)
  {
    std::ostringstream output;
    output << static_cast<double>(value) / 1000000.0;
    return output.str();
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

  std::string parent_directory(std::string const& path)
  {
    auto const separator = path.find_last_of('/');
    if (separator == std::string::npos || separator == 0U) {
      throw wcls::ipc::ProtocolError("worker path must be absolute");
    }
    return path.substr(0U, separator);
  }

  std::string filtered_library_path(std::string const& current)
  {
    std::string result;
    std::size_t begin = 0U;
    while (begin <= current.size()) {
      auto const end = current.find(':', begin);
      auto const item = current.substr(begin, end == std::string::npos ? end : end - begin);
      if (!item.empty() && item.find("/wirecell/") == std::string::npos &&
          item.find("build_larwirecell_streaming_recoframe/lib") == std::string::npos) {
        if (!result.empty()) {
          result.push_back(':');
        }
        result += item;
      }
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1U;
    }
    return result;
  }

  struct ExecEnvironment {
    std::vector<std::string> storage;
    std::vector<char*> pointers;
  };

  ExecEnvironment custom_worker_environment(std::string const& worker_path,
                                             std::string const& custom_wct_root,
                                             std::string const& process_c_python,
                                             std::string const& process_c_launcher,
                                             std::string const& process_c_host_lib64,
                                             std::string const& process_c_nugraph_root)
  {
    ExecEnvironment result;
    for (char** entry = environ; entry && *entry; ++entry) {
      std::string const value{*entry};
      if (value.rfind("LD_LIBRARY_PATH=", 0U) == 0U ||
          value.rfind("WIRECELL_PATH=", 0U) == 0U ||
          value.rfind("PROCESS_C_PYTHON=", 0U) == 0U ||
          value.rfind("PROCESS_C_LAUNCHER=", 0U) == 0U ||
          value.rfind("PROCESS_C_HOST_LIB64=", 0U) == 0U ||
          value.rfind("PROCESS_C_NUGRAPH_ROOT=", 0U) == 0U ||
          value.rfind("RECOFRAME_RUNTIME_ROLE=", 0U) == 0U) {
        continue;
      }
      result.storage.push_back(value);
    }

    auto const install_root = parent_directory(parent_directory(worker_path));
    auto const custom_opt = custom_wct_root + "/opt";
    auto const current_ld = std::getenv("LD_LIBRARY_PATH");
    auto const retained_ld = filtered_library_path(current_ld ? current_ld : "");
    auto library_path = install_root + "/lib:" + custom_opt + "/lib64:" + custom_opt + "/lib";
    if (!retained_ld.empty()) {
      library_path += ":" + retained_ld;
    }
    result.storage.push_back("LD_LIBRARY_PATH=" + library_path);
    result.storage.push_back("WIRECELL_PATH=" + custom_wct_root + "/cfg:" +
                             custom_wct_root + "/wire-cell-data");
    result.storage.push_back("RECOFRAME_RUNTIME_ROLE=custom-wct-worker");
    if (!process_c_python.empty() && !process_c_launcher.empty() &&
        !process_c_host_lib64.empty() && !process_c_nugraph_root.empty()) {
      result.storage.push_back("PROCESS_C_PYTHON=" + process_c_python);
      result.storage.push_back("PROCESS_C_LAUNCHER=" + process_c_launcher);
      result.storage.push_back("PROCESS_C_HOST_LIB64=" + process_c_host_lib64);
      result.storage.push_back("PROCESS_C_NUGRAPH_ROOT=" + process_c_nugraph_root);
    }
    result.pointers.reserve(result.storage.size() + 1U);
    for (auto& value : result.storage) {
      result.pointers.push_back(value.data());
    }
    result.pointers.push_back(nullptr);
    return result;
  }

  void mark_unrelated_fds_close_on_exec(int const preserved_fd)
  {
#if defined(SYS_close_range)
    constexpr unsigned int close_range_cloexec = 1U << 2U;
    if (::syscall(SYS_close_range, 3U, std::numeric_limits<unsigned int>::max(),
                  close_range_cloexec) == 0) {
      auto const flags = ::fcntl(preserved_fd, F_GETFD);
      if (flags >= 0) {
        ::fcntl(preserved_fd, F_SETFD, flags & ~FD_CLOEXEC);
      }
      return;
    }
#endif
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
      ::_exit(126);
    }
    auto const upper = static_cast<int>(std::min<rlim_t>(limit.rlim_cur,
                                                         std::numeric_limits<int>::max()));
    for (int fd = 3; fd < upper; ++fd) {
      if (fd == preserved_fd) {
        continue;
      }
      auto const flags = ::fcntl(fd, F_GETFD);
      if (flags >= 0) {
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
      }
    }
    auto const flags = ::fcntl(preserved_fd, F_GETFD);
    if (flags < 0 || ::fcntl(preserved_fd, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
      ::_exit(126);
    }
  }

  pid_t spawn_worker(int const parent_fd,
                     int const child_fd,
                     std::string const& worker_path,
                     ExecEnvironment& environment)
  {
    auto const fd_string = std::to_string(child_fd);
    std::vector<std::string> argument_storage{worker_path, "--ipc-fd", fd_string};
    std::vector<char*> arguments;
    for (auto& argument : argument_storage) {
      arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);

    auto const pid = ::fork();
    if (pid < 0) {
      throw wcls::ipc::ProtocolError(std::string{"fork failed: "} + std::strerror(errno));
    }
    if (pid == 0) {
      ::close(parent_fd);
      mark_unrelated_fds_close_on_exec(child_fd);
      ::execve(worker_path.c_str(), arguments.data(), environment.pointers.data());
      ::_exit(127);
    }
    return pid;
  }

  void require_identity(wcls::ipc::Message const& message,
                        wcls::EventRecoIdentity const& expected,
                        char const* const what)
  {
    auto const& actual = message.envelope.identity;
    if (actual.run != expected.run || actual.subrun != expected.subrun ||
        actual.event != expected.event) {
      fail(std::string{what} + " identity differs");
    }
  }

  void throw_worker_error(wcls::ipc::Message const& message)
  {
    if (message.envelope.message_type == wcls::ipc::MessageType::Error) {
      fail("worker error: " + wcls::ipc::decode_error_message(message));
    }
  }

  bool is_custom_core_library(std::string const& path, std::string const& custom_root)
  {
    return path.find(custom_root + "/opt/lib") != std::string::npos &&
           path.find("libWireCellLarsoft") == std::string::npos;
  }

  bool is_standard_core_library(std::string const& path)
  {
    return path.find("/software/products/wirecell/") != std::string::npos &&
           path.find("libWireCellLarsoft") == std::string::npos;
  }

} // namespace

namespace wcls {

  class RecoFramePayloadIpcEquivalence : public art::EDProducer {
  public:
    explicit RecoFramePayloadIpcEquivalence(fhicl::ParameterSet const& parameters)
      : EDProducer{parameters}
      , expected_run_{parameters.get<std::uint32_t>("expected_run")}
      , expected_subrun_{parameters.get<std::uint32_t>("expected_subrun")}
      , expected_event_{parameters.get<std::uint32_t>("expected_event")}
      , worker_path_{parameters.get<std::string>("worker_path")}
      , custom_wct_root_{parameters.get<std::string>("custom_wct_root")}
      , process_c_python_{parameters.get<std::string>("process_c_python", "")}
      , process_c_launcher_{parameters.get<std::string>("process_c_launcher", "")}
      , process_c_host_lib64_{parameters.get<std::string>("process_c_host_lib64", "")}
      , process_c_nugraph_root_{parameters.get<std::string>("process_c_nugraph_root", "")}
    {
      reference_source_.configure(historical_configuration());
    }

    void produce(art::Event& event) override
    {
      if (event.run() != expected_run_ || event.subRun() != expected_subrun_ ||
          event.event() != expected_event_) {
        fail("wrong golden event");
      }

      reference_source_.visit(event);
      WireCell::IFrame::pointer reference;
      WireCell::IFrame::pointer reference_eos;
      WireCell::IFrame::pointer reference_after_eos;
      if (!reference_source_(reference) || !reference || !reference_source_(reference_eos) ||
          reference_eos || reference_source_(reference_after_eos) || reference_after_eos) {
        fail("historical source contract is not frame, null/EOS, exhausted");
      }

      auto const payload = extract_reco_frame_payload(event);
      auto begin = Clock::now();
      auto const encoded_payload = ipc::encode_reco_frame_payload(payload);
      auto end = Clock::now();
      auto const encode_ns = nanoseconds(begin, end);

      int sockets[2] = {-1, -1};
      if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        fail(std::string{"socketpair failed: "} + std::strerror(errno));
      }
      timeval timeout{120, 0};
      if (::setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0 ||
          ::setsockopt(sockets[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) != 0) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        fail(std::string{"failed to set socket timeout: "} + std::strerror(errno));
      }

      auto environment = custom_worker_environment(worker_path_,
                                                   custom_wct_root_,
                                                   process_c_python_,
                                                   process_c_launcher_,
                                                   process_c_host_lib64_,
                                                   process_c_nugraph_root_);
      auto const child = spawn_worker(sockets[0], sockets[1], worker_path_, environment);
      ::close(sockets[1]);
      sockets[1] = -1;

      ipc::Message echo_message;
      ipc::Message report_message;
      ipc::Message frame_message;
      std::uint64_t socket_write_ns = 0U;
      std::uint64_t payload_round_trip_ns = 0U;
      try {
        begin = Clock::now();
        ipc::write_all(sockets[0], encoded_payload);
        auto const write_end = Clock::now();
        socket_write_ns = nanoseconds(begin, write_end);
        echo_message = ipc::read_message(sockets[0], ipc::DecodeLimits{}.max_message_payload_bytes);
        auto const echo_end = Clock::now();
        payload_round_trip_ns = nanoseconds(begin, echo_end);
        throw_worker_error(echo_message);
        report_message = ipc::read_message(sockets[0], 1024U * 1024U);
        throw_worker_error(report_message);
        frame_message = ipc::read_message(sockets[0], ipc::test::kMaxCanonicalIFrameBytes);
        throw_worker_error(frame_message);
      }
      catch (...) {
        ::close(sockets[0]);
        ::kill(child, SIGTERM);
        int ignored_status = 0;
        ::waitpid(child, &ignored_status, 0);
        throw;
      }
      ::close(sockets[0]);

      int child_status = 0;
      if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
          WEXITSTATUS(child_status) != 0) {
        fail("exec'd worker did not exit cleanly");
      }
      require_identity(echo_message, payload.identity, "echo");
      require_identity(report_message, payload.identity, "worker report");
      require_identity(frame_message, payload.identity, "canonical IFrame");

      begin = Clock::now();
      auto const returned_payload = ipc::decode_reco_frame_payload(echo_message);
      end = Clock::now();
      auto const parent_decode_ns = nanoseconds(begin, end);
      auto const canonical_echo = ipc::encode_reco_frame_payload(returned_payload);
      if (canonical_echo != encoded_payload) {
        fail("worker RecoFramePayload echo is not byte-for-byte canonical");
      }

      auto const report = ipc::test::decode_worker_report(report_message);
      if (frame_message.envelope.message_type != ipc::MessageType::TestIFrame) {
        fail("worker returned wrong canonical-IFrame message type");
      }
      begin = Clock::now();
      auto const reference_frame_bytes = ipc::test::encode_canonical_iframe(*reference);
      end = Clock::now();
      auto const parent_frame_encode_ns = nanoseconds(begin, end);
      if (reference_frame_bytes != frame_message.payload) {
        auto const common = std::mismatch(reference_frame_bytes.begin(),
                                          reference_frame_bytes.end(),
                                          frame_message.payload.begin(),
                                          frame_message.payload.end());
        auto const offset = static_cast<std::size_t>(common.first - reference_frame_bytes.begin());
        fail("cross-process canonical IFrame differs at byte " + std::to_string(offset));
      }

      auto const reference_fingerprint = ipc::test::fingerprint_bytes(reference_frame_bytes);
      auto const child_fingerprint = ipc::test::fingerprint_bytes(frame_message.payload);
      if (reference_fingerprint != child_fingerprint || child_fingerprint != report.frame_fingerprint) {
        fail("cross-process IFrame fingerprints differ after exact field comparison");
      }

      auto const parent_libraries = ipc::test::loaded_wirecell_libraries();
      bool parent_has_standard = false;
      bool parent_has_custom = false;
      for (auto const& library : parent_libraries) {
        parent_has_standard = parent_has_standard || is_standard_core_library(library);
        parent_has_custom = parent_has_custom || is_custom_core_library(library, custom_wct_root_);
        std::cout << "RECOFRAME_IPC_PARENT_LIBRARY " << library << '\n';
      }
      bool child_has_standard = false;
      bool child_has_custom = false;
      for (auto const& library : report.loaded_wirecell_libraries) {
        child_has_standard = child_has_standard || is_standard_core_library(library);
        child_has_custom = child_has_custom || is_custom_core_library(library, custom_wct_root_);
        std::cout << "RECOFRAME_IPC_CHILD_LIBRARY " << library << '\n';
      }
      if (!parent_has_standard || parent_has_custom || !child_has_custom || child_has_standard) {
        fail("parent/child Wire-Cell runtime isolation check failed");
      }

      std::cout << "RECOFRAME_IPC_EQUIVALENCE PASS event=" << payload.identity.run << "/"
                << payload.identity.subrun << "/" << payload.identity.event
                << " reference_fingerprint=" << reference_fingerprint
                << " child_fingerprint=" << child_fingerprint
                << " logical_payload_bytes=" << reco_frame_payload_bytes(payload)
                << " encoded_wire_bytes=" << encoded_payload.size()
                << " canonical_frame_bytes=" << frame_message.payload.size()
                << " encode_ms=" << milliseconds(encode_ns)
                << " socket_write_ms=" << milliseconds(socket_write_ns)
                << " payload_round_trip_ms=" << milliseconds(payload_round_trip_ns)
                << " child_decode_ms=" << milliseconds(report.decode_nanoseconds)
                << " parent_echo_decode_ms=" << milliseconds(parent_decode_ns)
                << " child_reencode_ms=" << milliseconds(report.reencode_nanoseconds)
                << " child_rebuild_ms=" << milliseconds(report.rebuild_nanoseconds)
                << " child_frame_encode_ms=" << milliseconds(report.frame_encode_nanoseconds)
                << " parent_frame_encode_ms=" << milliseconds(parent_frame_encode_ns)
                << std::endl;
    }

  private:
    std::uint32_t expected_run_;
    std::uint32_t expected_subrun_;
    std::uint32_t expected_event_;
    std::string worker_path_;
    std::string custom_wct_root_;
    std::string process_c_python_;
    std::string process_c_launcher_;
    std::string process_c_host_lib64_;
    std::string process_c_nugraph_root_;
    CookedFrameSource reference_source_;
  };

} // namespace wcls

DEFINE_ART_MODULE(wcls::RecoFramePayloadIpcEquivalence)
