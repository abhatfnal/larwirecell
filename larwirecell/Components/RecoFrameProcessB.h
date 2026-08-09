#ifndef LARWIRECELL_COMPONENTS_RECOFRAMEPROCESSB_H
#define LARWIRECELL_COMPONENTS_RECOFRAMEPROCESSB_H

#include "RecoArrays.h"
#include "RecoFramePayload.h"

#include "WireCellIface/ITensorSet.h"
#include "WireCellUtil/Configuration.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace wcls {

  struct ProcessBApaResult {
    bool received = false;
    bool saw_eos = false;
    bool memory_clustering_completed = false;
    bool legacy_equivalent = false;
    std::int32_t clustering_tensor_ident = 0;
    std::int32_t tensor_ident = 0;
    RecoArrays reco_arrays;
  };

  struct ProcessBResult {
    EventRecoIdentity identity;
    std::array<ProcessBApaResult, 2U> apa;
  };

  using ProcessBMemoryClusteringCallback =
    std::function<bool(WireCell::Configuration const&,
                       WireCell::ITensorSet::pointer const&,
                       WireCell::ITensorSet::pointer&)>;
  using ProcessBRecoArraysSinkCallback =
    std::function<bool(int,
                       WireCell::Configuration const&,
                       WireCell::ITensorSet::pointer const&)>;

  // The existing WireCellLarsoft plugin owns only interface-level proxies.
  // Process B installs executable-side implementations before WCT starts, so
  // the plugin gains no Aux/Clus or plugin-to-plugin dependency.
  void install_process_b_callbacks(ProcessBMemoryClusteringCallback clustering,
                                   ProcessBRecoArraysSinkCallback sink);
  void install_process_b_runtime();

  // Install one event in the process-local source/sink registry before WCT
  // components are constructed.  legacy_compare_directory must be empty for
  // the no-file Process-B path.  A non-empty value enables only the explicitly
  // allowed development boundary comparison.
  void prepare_process_b(RecoFramePayload const& payload,
                         std::string const& legacy_compare_directory = {});

  // Called only by the memory-only clustering shell after it has produced one
  // final tensor set without executing an event-data writer.
  void record_process_b_memory_clustering(int apa, std::int32_t tensor_ident);

  std::string process_b_legacy_compare_directory();
  void record_process_b_sink_eos(int apa);
  void store_process_b_reco_arrays(int apa,
                                   std::int32_t tensor_ident,
                                   RecoArrays arrays,
                                   bool legacy_equivalent);

  bool run_process_b_memory_clustering(WireCell::Configuration const& config,
                                       WireCell::ITensorSet::pointer const& input,
                                       WireCell::ITensorSet::pointer& output);
  bool consume_process_b_reco_arrays(int apa,
                                     WireCell::Configuration const& config,
                                     WireCell::ITensorSet::pointer const& input);

  // Require and move out one complete two-APA event transaction.
  ProcessBResult take_process_b_result();

} // namespace wcls

#endif
