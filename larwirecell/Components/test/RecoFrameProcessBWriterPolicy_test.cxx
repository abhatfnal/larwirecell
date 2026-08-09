#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

  std::string read(std::string const& path)
  {
    std::ifstream input{path};
    if (!input) {
      throw std::runtime_error("cannot read policy input: " + path);
    }
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
  }

  void require_contains(std::string const& content,
                        std::string const& needle,
                        std::string const& description)
  {
    if (content.find(needle) == std::string::npos) {
      throw std::runtime_error("missing " + description + ": " + needle);
    }
  }

  void require_absent(std::string const& content,
                      std::string const& needle,
                      std::string const& description)
  {
    if (content.find(needle) != std::string::npos) {
      throw std::runtime_error("forbidden " + description + ": " + needle);
    }
  }

} // namespace

int main(int const argc, char const* const* const argv)
{
  try {
    if (argc != 7) {
      throw std::runtime_error(
        "expected graph, worker, memory-clustering, proxy-component, CMake, and "
        "Process-C launcher paths");
    }
    auto const graph = read(argv[1]);
    auto const worker = read(argv[2]);
    auto const memory_clustering = read(argv[3]);
    auto const proxy_components = read(argv[4]);
    auto const cmake = read(argv[5]);
    auto const process_c_launcher = read(argv[6]);

    require_contains(graph,
                     "wclsMemoryMultiAlgBlobClustering",
                     "memory-only clustering component");
    require_contains(graph,
                     "local memory_node_label(node)",
                     "memory-only Pgrapher node-label mapping");
    require_contains(graph,
                     "edges: std.map(memory_only_edge, g.edges(graph))",
                     "memory-only Pgrapher edge mapping");
    for (auto const& forbidden : {"process_b_bee_sink", "bee_zip", "/dev/null"}) {
      require_absent(graph, forbidden, "event-writer graph configuration");
      require_absent(worker, forbidden, "event-writer worker adapter");
    }
    require_contains(worker,
                     "value.rfind(\"PYTHONHOME=\", 0U)",
                     "Process-C Python-home isolation");
    require_contains(worker,
                     "PYTHONNOUSERSITE=1",
                     "Process-C user-site isolation");
    require_contains(worker,
                     "PROCESS_C_LAUNCHER",
                     "Process-C cross-runtime launcher selection");
    require_contains(process_c_launcher,
                     "ld-linux-x86-64.so.2",
                     "Process-C modern-glibc loader");
    require_contains(process_c_launcher,
                     "exec \"${host_loader}\"",
                     "immediate Process-C Python exec");
    require_contains(graph, "\"WireCellLarsoft\"", "interface-only proxy plugin");
    for (auto const& factory : {"make_wclsRecoFrameProcessBSource_factory()",
                                "make_wclsRecoArraysProcessBSink_factory()",
                                "make_wclsMemoryMultiAlgBlobClustering_factory()"}) {
      require_absent(worker, factory, "executable-local WCT factory registration");
    }
    for (auto const& forbidden : {"WireCellUtil/Bee.h", "Bee::Sink", "grouping2file"}) {
      require_absent(memory_clustering, forbidden, "event writer in memory clustering");
    }

    require_contains(cmake,
                     "SOURCE RecoFrameProcessBWorker.cxx RecoFrameProcessB.cxx RecoArrays.cxx "
                     "MemoryMultiAlgBlobClustering.cxx",
                     "Process-B-only executable sources");
    require_contains(cmake,
                     "  RecoFrameProcessBComponents.cxx\n",
                     "interface-only Process-B proxy plugin source");
    require_absent(cmake,
                   "  RecoFrameProcessB.cxx\n",
                   "Process-B component in WireCellLarsoft plugin sources");
    require_absent(cmake,
                   "  RecoArrays.cxx\n",
                   "RecoArrays helper in WireCellLarsoft plugin sources");
    require_contains(cmake,
                     "process_c_python311_launcher.sh",
                     "installed Process-C launcher");

    for (auto const& forbidden : {"WireCellAux/", "WireCellClus/", "WireCellUtil/Bee.h"}) {
      require_absent(proxy_components, forbidden, "proxy plugin implementation dependency");
    }
    auto const executable_marker = cmake.find("cet_make_exec(NAME reco_frame_ipc_worker");
    if (executable_marker == std::string::npos) {
      throw std::runtime_error("cannot identify executable section in CMake");
    }
    auto const plugin_cmake = cmake.substr(0U, executable_marker);
    require_absent(plugin_cmake, "WireCell::Clus", "Clus link in WireCellLarsoft plugin");
    require_absent(plugin_cmake, "WireCell::Aux", "Aux link in WireCellLarsoft plugin");

    std::cout << "PROCESS_B_WRITER_POLICY PASS\n";
    return 0;
  }
  catch (std::exception const& error) {
    std::cerr << "PROCESS_B_WRITER_POLICY FAIL: " << error.what() << '\n';
    return 1;
  }
}
