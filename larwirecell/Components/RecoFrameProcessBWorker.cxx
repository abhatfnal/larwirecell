#include "RecoFrameIpcTestSupport.h"
#include "RecoFrameBuilder.h"
#include "RecoFrameProcessB.h"
#include "RecoFrameProtocol.h"

#include "WireCellApps/Main.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace {

  using Clock = std::chrono::steady_clock;

  std::string milliseconds(std::uint64_t const nanoseconds)
  {
    std::ostringstream output;
    output << static_cast<double>(nanoseconds) / 1000000.0;
    return output.str();
  }

  std::uint64_t elapsed(Clock::time_point const begin, Clock::time_point const end)
  {
    return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
  }

  struct WorkerArgs {
    int fd = -1;
    bool parent_mode = false;
  };

  int parse_ipc_fd(char const* const value)
  {
    errno = 0;
    char* end = nullptr;
    auto const parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 0 ||
        parsed > std::numeric_limits<int>::max()) {
      throw wcls::ipc::ProtocolError("invalid --ipc-fd value");
    }
    return static_cast<int>(parsed);
  }

  [[noreturn]] void throw_usage()
  {
    throw wcls::ipc::ProtocolError(
      "usage: reco_frame_process_b_worker --ipc-fd N [--output-mode test-python|parent]");
  }

  WorkerArgs parse_args(int const argc, char const* const* const argv)
  {
    if (argc != 3 && argc != 5) {
      throw_usage();
    }
    int fd = -1;
    bool has_fd = false;
    bool has_mode = false;
    bool parent_mode = false;
    for (int i = 1; i + 1 < argc; i += 2) {
      std::string const key{argv[i]};
      std::string const value{argv[i + 1]};
      if (key == "--ipc-fd") {
        if (has_fd) {
          throw_usage();
        }
        fd = parse_ipc_fd(value.c_str());
        has_fd = true;
      } else if (key == "--output-mode") {
        if (has_mode) {
          throw_usage();
        }
        if (value == "parent") {
          parent_mode = true;
        } else if (value == "test-python") {
          parent_mode = false;
        } else {
          throw wcls::ipc::ProtocolError(
            "usage: reco_frame_process_b_worker --ipc-fd N [--output-mode test-python|parent]");
        }
        has_mode = true;
      } else {
        throw_usage();
      }
    }
    if (!has_fd) {
      throw_usage();
    }
    return {fd, parent_mode};
  }

  std::filesystem::path executable_path()
  {
    std::vector<char> buffer(4096U, '\0');
    auto const length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (length < 0 || static_cast<std::size_t>(length) >= buffer.size() - 1U) {
      throw std::runtime_error("failed to resolve Process-B executable path");
    }
    return std::filesystem::path{std::string{buffer.data(), static_cast<std::size_t>(length)}};
  }

  std::vector<std::string> split_path(char const* const value)
  {
    std::vector<std::string> result;
    if (!value) {
      return result;
    }
    std::string const input{value};
    std::size_t begin = 0U;
    while (begin <= input.size()) {
      auto const end = input.find(':', begin);
      auto const item = input.substr(begin, end == std::string::npos ? end : end - begin);
      if (!item.empty()) {
        result.push_back(item);
      }
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1U;
    }
    return result;
  }

  std::string required_environment(char const* const name)
  {
    auto const* value = std::getenv(name);
    if (!value || !*value) {
      throw std::runtime_error(std::string{"missing Process-C environment: "} + name);
    }
    return value;
  }

  struct ExecEnvironment {
    std::vector<std::string> storage;
    std::vector<char*> pointers;
  };

  ExecEnvironment process_c_environment(std::string const& nugraph_root,
                                        std::string const& host_lib64)
  {
    ExecEnvironment result;
    for (char** entry = environ; entry && *entry; ++entry) {
      std::string const value{*entry};
      if (value.rfind("LD_LIBRARY_PATH=", 0U) == 0U ||
          value.rfind("WIRECELL_PATH=", 0U) == 0U ||
          value.rfind("CET_PLUGIN_PATH=", 0U) == 0U ||
          value.rfind("FHICL_FILE_PATH=", 0U) == 0U ||
          value.rfind("PYTHONHOME=", 0U) == 0U ||
          value.rfind("PYTHONUSERBASE=", 0U) == 0U ||
          value.rfind("PYTHONNOUSERSITE=", 0U) == 0U ||
          value.rfind("PYTHONPATH=", 0U) == 0U ||
          value.rfind("PROCESS_C_PYTHON=", 0U) == 0U ||
          value.rfind("PROCESS_C_LAUNCHER=", 0U) == 0U ||
          value.rfind("PROCESS_C_HOST_LIB64=", 0U) == 0U ||
          value.rfind("PROCESS_C_NUGRAPH_ROOT=", 0U) == 0U ||
          value.rfind("RECOFRAME_RUNTIME_ROLE=", 0U) == 0U ||
          value.rfind("PROCESS_C_RUNTIME_ROLE=", 0U) == 0U) {
        continue;
      }
      result.storage.push_back(value);
    }
    result.storage.push_back("PYTHONPATH=" + nugraph_root);
    result.storage.push_back("PYTHONNOUSERSITE=1");
    result.storage.push_back("PROCESS_C_HOST_LIB64=" + host_lib64);
    result.storage.push_back("PROCESS_C_RUNTIME_ROLE=python311-nugraph");
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

  pid_t spawn_process_c(int const parent_fd,
                        int const child_fd,
                        std::string const& python,
                        std::string const& launcher,
                        std::string const& script,
                        std::string const& nugraph_root,
                        ExecEnvironment& environment)
  {
    auto const fd_string = std::to_string(child_fd);
    std::vector<std::string> argument_storage{
      launcher, python, script, "--ipc-fd", fd_string, "--nugraph-root", nugraph_root};
    std::vector<char*> arguments;
    for (auto& argument : argument_storage) {
      arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);

    auto const pid = ::fork();
    if (pid < 0) {
      throw std::runtime_error(std::string{"Process-C fork failed: "} + std::strerror(errno));
    }
    if (pid == 0) {
      ::close(parent_fd);
      mark_unrelated_fds_close_on_exec(child_fd);
      ::execve(launcher.c_str(), arguments.data(), environment.pointers.data());
      ::_exit(127);
    }
    return pid;
  }

  std::string custom_wct_root()
  {
    auto const paths = split_path(std::getenv("WIRECELL_PATH"));
    for (auto const& path : paths) {
      constexpr char suffix[] = "/cfg";
      if (path.size() > sizeof suffix - 1U &&
          path.compare(path.size() - (sizeof suffix - 1U), sizeof suffix - 1U, suffix) == 0) {
        return path.substr(0U, path.size() - (sizeof suffix - 1U));
      }
    }
    throw std::runtime_error("WIRECELL_PATH does not identify the custom WCT root");
  }

  std::string make_legacy_compare_directory()
  {
    auto const* enabled = std::getenv("PROCESS_B_LEGACY_COMPARE");
    if (!enabled || std::string{enabled} != "1") {
      return {};
    }
    std::string pattern = "/tmp/recoframe-process-b-boundary-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    auto const* created = ::mkdtemp(writable.data());
    if (!created) {
      throw std::runtime_error("failed to create test-only boundary comparison directory");
    }
    return created;
  }

  void run_process_b_graph(std::filesystem::path const& config_path)
  {
    WireCell::Main main;
    for (auto const& path : split_path(std::getenv("WIRECELL_PATH"))) {
      main.add_path(path);
    }
    main.add_var("reality", "data");
    main.add_config(config_path.string());
    main.add_logsink("stdout", "debug");
    main.set_loglevel("", "debug");
    main.initialize();
    main();
  }

  void require_exact_clustering_runtime(std::vector<std::string> const& libraries,
                                        std::string const& root)
  {
    auto const expected_prefix = root + "/opt/lib";
    std::size_t clus_count = 0U;
    for (auto const& library : libraries) {
      if (library.find("libWireCellClus.so") == std::string::npos) {
        continue;
      }
      ++clus_count;
      if (library.find(expected_prefix) == std::string::npos ||
          library.find("/software/products/wirecell/") != std::string::npos ||
          library.find("wct-streaming-recoarrays") != std::string::npos) {
        throw std::runtime_error("Process B loaded a non-archived libWireCellClus.so: " + library);
      }
    }
    if (clus_count != 1U) {
      throw std::runtime_error("Process B did not load exactly one libWireCellClus.so");
    }
  }

  void require_identity(wcls::ipc::Message const& message,
                        wcls::EventRecoIdentity const& expected,
                        char const* const context)
  {
    auto const& actual = message.envelope.identity;
    if (actual.run != expected.run || actual.subrun != expected.subrun ||
        actual.event != expected.event) {
      throw std::runtime_error(std::string{context} + " event identity differs");
    }
  }

  void throw_process_c_error(wcls::ipc::Message const& message)
  {
    if (message.envelope.message_type == wcls::ipc::MessageType::Error) {
      throw std::runtime_error("Process C rejected RecoArrays: " +
                               wcls::ipc::decode_error_message(message));
    }
  }

  void require_same_reco_arrays(wcls::RecoArrays const& source,
                                wcls::RecoArrays const& returned,
                                int const apa)
  {
    if (source.arrays.size() != returned.arrays.size()) {
      throw std::runtime_error("Process-C APA" + std::to_string(apa) +
                               " array count differs");
    }
    for (std::size_t array_index = 0U; array_index < source.arrays.size(); ++array_index) {
      auto const& expected = source.arrays[array_index];
      auto const& actual = returned.arrays[array_index];
      if (expected.name != actual.name || expected.rows != actual.rows ||
          expected.columns != actual.columns || expected.values.size() != actual.values.size()) {
        throw std::runtime_error("Process-C APA" + std::to_string(apa) +
                                 " metadata differs for " + expected.name);
      }
      auto const byte_count = expected.values.size() * sizeof(float);
      if (byte_count == 0U ||
          std::memcmp(expected.values.data(), actual.values.data(), byte_count) == 0) {
        continue;
      }
      auto const* expected_bytes =
        reinterpret_cast<unsigned char const*>(expected.values.data());
      auto const* actual_bytes = reinterpret_cast<unsigned char const*>(actual.values.data());
      auto const mismatch =
        std::mismatch(expected_bytes, expected_bytes + byte_count, actual_bytes);
      auto const byte_index = static_cast<std::size_t>(mismatch.first - expected_bytes);
      auto const element_index = byte_index / sizeof(float);
      std::uint32_t expected_bits = 0U;
      std::uint32_t actual_bits = 0U;
      std::memcpy(&expected_bits, expected.values.data() + element_index, sizeof expected_bits);
      std::memcpy(&actual_bits, actual.values.data() + element_index, sizeof actual_bits);
      throw std::runtime_error("Process-C APA" + std::to_string(apa) + " raw bytes differ for " +
                               expected.name + " at byte " + std::to_string(byte_index) +
                               " element " + std::to_string(element_index) + " C++ bits " +
                               std::to_string(expected_bits) + " Python bits " +
                               std::to_string(actual_bits));
    }
  }

  void run_process_c(wcls::ProcessBResult const& result)
  {
    auto const python = required_environment("PROCESS_C_PYTHON");
    auto const launcher = required_environment("PROCESS_C_LAUNCHER");
    auto const host_lib64 = required_environment("PROCESS_C_HOST_LIB64");
    auto const nugraph_root = required_environment("PROCESS_C_NUGRAPH_ROOT");
    auto const script = nugraph_root + "/scripts/process_c_recoarrays_receiver.py";
    if (!std::filesystem::is_regular_file(python) ||
        !std::filesystem::is_regular_file(launcher) ||
        !std::filesystem::is_regular_file(host_lib64 + "/ld-linux-x86-64.so.2") ||
        !std::filesystem::is_regular_file(script)) {
      throw std::runtime_error("Process-C Python, launcher, host loader, or receiver is missing");
    }

    std::array<std::vector<std::uint8_t>, 2U> encoded;
    auto const encode_begin = Clock::now();
    for (std::size_t apa = 0U; apa < encoded.size(); ++apa) {
      auto const& arrays = result.apa[apa].reco_arrays;
      encoded[apa] = wcls::ipc::encode_reco_arrays(result.identity,
                                                   static_cast<int>(apa),
                                                   arrays);
      for (auto const& array : arrays.arrays) {
        std::cout << "PROCESS_B_RECOARRAY_FINGERPRINT apa=" << apa
                  << " name=" << array.name
                  << " value=" << wcls::ipc::fingerprint_reco_array(array) << '\n';
      }
    }
    auto const encode_ns = elapsed(encode_begin, Clock::now());

    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
      throw std::runtime_error(std::string{"Process-C socketpair failed: "} +
                               std::strerror(errno));
    }
    timeval timeout{120, 0};
    if (::setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0 ||
        ::setsockopt(sockets[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) != 0) {
      ::close(sockets[0]);
      ::close(sockets[1]);
      throw std::runtime_error("failed to set Process-C socket timeout");
    }

    auto environment = process_c_environment(nugraph_root, host_lib64);
    auto const child = spawn_process_c(
      sockets[0], sockets[1], python, launcher, script, nugraph_root, environment);
    ::close(sockets[1]);
    sockets[1] = -1;
    auto const round_trip_begin = Clock::now();
    std::uint64_t socket_write_ns = 0U;
    try {
      auto const write_begin = Clock::now();
      for (auto const& message : encoded) {
        wcls::ipc::write_all(sockets[0], message);
      }
      wcls::ipc::MessageEnvelope end;
      end.message_type = wcls::ipc::MessageType::EndOfStream;
      end.identity = result.identity;
      wcls::ipc::write_all(sockets[0], wcls::ipc::encode_message(end, {}));
      socket_write_ns = elapsed(write_begin, Clock::now());

      for (std::size_t apa = 0U; apa < encoded.size(); ++apa) {
        auto const response = wcls::ipc::read_message(
          sockets[0], wcls::ipc::RecoArraysDecodeLimits{}.max_message_payload_bytes);
        throw_process_c_error(response);
        require_identity(response, result.identity, "Process-C RecoArrays response");
        auto const canonical = wcls::ipc::encode_message(response.envelope, response.payload);
        if (canonical != encoded[apa]) {
          auto const common = std::min(canonical.size(), encoded[apa].size());
          auto const mismatch = std::mismatch(encoded[apa].begin(),
                                              encoded[apa].begin() + common,
                                              canonical.begin());
          auto const offset = static_cast<std::size_t>(mismatch.first - encoded[apa].begin());
          throw std::runtime_error("Process-C APA" + std::to_string(apa) +
                                   " canonical message differs at byte " +
                                   std::to_string(offset));
        }
        auto const returned = wcls::ipc::decode_reco_arrays(response, static_cast<int>(apa));
        require_same_reco_arrays(result.apa[apa].reco_arrays, returned, static_cast<int>(apa));
        std::cout << "PROCESS_B_TO_C_RECOARRAYS apa=" << apa
                  << " same_run_cpp_python=true raw_bytes=true canonical_round_trip=true"
                  << " wire_bytes=" << encoded[apa].size() << '\n';
      }

      auto const ack = wcls::ipc::read_message(sockets[0], 4096U);
      throw_process_c_error(ack);
      require_identity(ack, result.identity, "Process-C acknowledgement");
      if (ack.envelope.message_type != wcls::ipc::MessageType::Ack || !ack.payload.empty()) {
        throw std::runtime_error("Process C returned an invalid acknowledgement");
      }
      ::close(sockets[0]);
      sockets[0] = -1;
    }
    catch (...) {
      if (sockets[0] >= 0) {
        ::close(sockets[0]);
      }
      ::kill(child, SIGTERM);
      int ignored_status = 0;
      ::waitpid(child, &ignored_status, 0);
      throw;
    }

    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      throw std::runtime_error("exec'd Process C did not exit cleanly");
    }
    auto const round_trip_ns = elapsed(round_trip_begin, Clock::now());
    std::cout << "PROCESS_B_TO_C_TRANSACTION PASS event=" << result.identity.run << "/"
              << result.identity.subrun << "/" << result.identity.event
              << " apa0=true apa1=true"
              << " apa0_wire_bytes=" << encoded[0].size()
              << " apa1_wire_bytes=" << encoded[1].size()
              << " cpp_encode_ms=" << milliseconds(encode_ns)
              << " socket_write_ms=" << milliseconds(socket_write_ns)
              << " round_trip_ms=" << milliseconds(round_trip_ns) << '\n';
  }

  void run_parent_return(wcls::ProcessBResult const& result, int const fd)
  {
    std::array<std::vector<std::uint8_t>, 2U> encoded;
    for (std::size_t apa = 0U; apa < encoded.size(); ++apa) {
      encoded[apa] = wcls::ipc::encode_reco_arrays(result.identity,
                                                    static_cast<int>(apa),
                                                    result.apa[apa].reco_arrays);
      std::cout << "PROCESS_B_RECOARRAYS_WIRE apa=" << apa
                << " wire_bytes=" << encoded[apa].size()
                << " fingerprint=" << wcls::ipc::test::fingerprint_bytes(encoded[apa]) << '\n';
    }
    wcls::ipc::write_all(fd, encoded[0]);
    wcls::ipc::write_all(fd, encoded[1]);
    wcls::ipc::MessageEnvelope eos;
    eos.message_type = wcls::ipc::MessageType::EndOfStream;
    eos.identity = result.identity;
    wcls::ipc::write_all(fd, wcls::ipc::encode_message(eos, {}));
    auto const ack = wcls::ipc::read_message(fd, 4096U);
    if (ack.envelope.message_type != wcls::ipc::MessageType::Ack || !ack.payload.empty()) {
      throw std::runtime_error("parent Ack message is invalid");
    }
    require_identity(ack, result.identity, "parent Ack");
    std::cout << "PROCESS_B_PARENT_RETURN PASS apa0=true apa1=true ack=true\n";
  }

  void report_arrays(wcls::ProcessBResult const& result, bool const compared)
  {
    for (std::size_t apa = 0U; apa < result.apa.size(); ++apa) {
      auto const& lane = result.apa[apa];
      if (lane.reco_arrays.arrays.size() != 6U) {
        throw std::runtime_error("Process-B APA does not have the six-array reconstruction contract");
      }
      std::cout << "PROCESS_B_RECOARRAYS apa=" << apa
                << " tensor_ident=" << lane.tensor_ident
                << " memory_clustering="
                << (lane.memory_clustering_completed ? "true" : "false")
                << " legacy_equivalent=" << (lane.legacy_equivalent ? "true" : "not-run")
                << " arrays=" << lane.reco_arrays.arrays.size();
      for (auto const& array : lane.reco_arrays.arrays) {
        std::cout << " " << array.name << "=[" << array.rows << "," << array.columns
                  << "]f4:" << array.values.size() * sizeof(float);
      }
      std::cout << '\n';
      if (compared && !lane.legacy_equivalent) {
        throw std::runtime_error("Process-B boundary comparison was requested but did not pass");
      }
    }
    std::cout << "PROCESS_B_EVENT_TRANSACTION PASS event=" << result.identity.run << "/"
              << result.identity.subrun << "/" << result.identity.event
              << " apa0_ident=" << result.apa[0].tensor_ident
              << " apa1_ident=" << result.apa[1].tensor_ident << '\n';
    std::cout << "PROCESS_B_EVENT_WRITER_AUDIT production_graph_bee=false"
              << " production_graph_grouping2file=false"
              << " tensor_file_sink=false redirected_writer=false"
              << " boundary_reference_npz=" << (compared ? "true" : "false") << '\n';
  }

} // namespace

int main(int const argc, char const* const* const argv)
{
  int fd = -1;
  wcls::EventRecoIdentity identity;
  std::string legacy_directory;
  try {
    auto const args = parse_args(argc, argv);
    fd = args.fd;
    wcls::install_process_b_runtime();
    auto const incoming =
      wcls::ipc::read_message(fd, wcls::ipc::DecodeLimits{}.max_message_payload_bytes);
    identity = incoming.envelope.identity;
    auto const payload = wcls::ipc::decode_reco_frame_payload(incoming);

    // Preserve the accepted payload-only echo contract and prove the exact
    // incoming IFrame before the graph consumes it.
    auto const echo = wcls::ipc::encode_reco_frame_payload(payload);
    wcls::ipc::write_all(fd, echo);
    auto const frame = wcls::make_reco_frame(payload);
    auto const canonical_frame = wcls::ipc::test::encode_canonical_iframe(*frame);

    legacy_directory = make_legacy_compare_directory();
    wcls::prepare_process_b(payload, legacy_directory);

    auto const install_root = executable_path().parent_path().parent_path();
    auto const config_path = install_root / "fcl" / "reco-frame-process-b.jsonnet";
    if (!std::filesystem::is_regular_file(config_path)) {
      throw std::runtime_error("installed Process-B graph configuration is missing");
    }
    run_process_b_graph(config_path);
    auto const result = wcls::take_process_b_result();
    report_arrays(result, !legacy_directory.empty());

    if (args.parent_mode) {
      run_parent_return(result, fd);
    } else {
      run_process_c(result);
    }

    auto const libraries = wcls::ipc::test::loaded_wirecell_libraries();
    require_exact_clustering_runtime(libraries, custom_wct_root());
    for (auto const& library : libraries) {
      std::cout << "PROCESS_B_LIBRARY " << library << '\n';
    }

    if (args.parent_mode) {
      std::cout << "PROCESS_B_OUTPUT_MODE mode=parent apa0=true apa1=true committed=true\n";
      ::close(fd);
      if (!legacy_directory.empty()) {
        std::filesystem::remove_all(legacy_directory);
      }
      return 0;
    }

    wcls::ipc::test::WorkerReport report;
    report.frame_fingerprint = wcls::ipc::test::fingerprint_bytes(canonical_frame);
    report.loaded_wirecell_libraries = libraries;
    auto const encoded_report = wcls::ipc::test::encode_worker_report(payload.identity, report);

    wcls::ipc::MessageEnvelope frame_envelope;
    frame_envelope.message_type = wcls::ipc::MessageType::TestIFrame;
    frame_envelope.identity = payload.identity;
    auto const encoded_frame = wcls::ipc::encode_message(frame_envelope, canonical_frame);
    wcls::ipc::write_all(fd, encoded_report);
    wcls::ipc::write_all(fd, encoded_frame);
    ::close(fd);
    if (!legacy_directory.empty()) {
      std::filesystem::remove_all(legacy_directory);
    }
    return 0;
  }
  catch (std::exception const& error) {
    if (fd >= 0) {
      try {
        wcls::ipc::write_all(fd, wcls::ipc::encode_error_message(identity, error.what()));
      }
      catch (...) {
      }
      ::close(fd);
    }
    if (!legacy_directory.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(legacy_directory, ignored);
    }
    std::cerr << "reco_frame_process_b_worker: " << error.what() << '\n';
    return 2;
  }
}
