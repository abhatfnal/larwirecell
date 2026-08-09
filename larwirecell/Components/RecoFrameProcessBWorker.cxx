#include "RecoFrameIpcTestSupport.h"
#include "RecoFrameBuilder.h"
#include "RecoFrameProcessB.h"
#include "RecoFrameProtocol.h"

#include "WireCellApps/Main.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  int parse_fd(int const argc, char const* const* const argv)
  {
    if (argc != 3 || std::string{argv[1]} != "--ipc-fd") {
      throw wcls::ipc::ProtocolError("usage: reco_frame_process_b_worker --ipc-fd N");
    }
    errno = 0;
    char* end = nullptr;
    auto const parsed = std::strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || parsed < 0 ||
        parsed > std::numeric_limits<int>::max()) {
      throw wcls::ipc::ProtocolError("invalid --ipc-fd value");
    }
    return static_cast<int>(parsed);
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
    fd = parse_fd(argc, argv);
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

    auto const libraries = wcls::ipc::test::loaded_wirecell_libraries();
    require_exact_clustering_runtime(libraries, custom_wct_root());
    for (auto const& library : libraries) {
      std::cout << "PROCESS_B_LIBRARY " << library << '\n';
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
