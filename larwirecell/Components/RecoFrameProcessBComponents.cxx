#include "RecoFrameProcessB.h"

#include "RecoFrameBuilder.h"

#include "WireCellIface/IConfigurable.h"
#include "WireCellIface/IFrameSource.h"
#include "WireCellIface/ITensorSetFilter.h"
#include "WireCellIface/ITensorSetSink.h"
#include "WireCellUtil/NamedFactory.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace {

  struct Registry {
    std::mutex mutex;
    WireCell::IFrame::pointer frame;
    wcls::ProcessBResult result;
    std::string legacy_compare_directory;
    wcls::ProcessBMemoryClusteringCallback clustering_callback;
    wcls::ProcessBRecoArraysSinkCallback sink_callback;
    bool prepared = false;
    bool taken = false;
  };

  Registry& registry()
  {
    static Registry instance;
    return instance;
  }

  wcls::ProcessBMemoryClusteringCallback clustering_callback()
  {
    auto& state = registry();
    std::lock_guard<std::mutex> lock{state.mutex};
    if (!state.clustering_callback) {
      throw std::logic_error("Process-B memory clustering callback is not installed");
    }
    return state.clustering_callback;
  }

  wcls::ProcessBRecoArraysSinkCallback sink_callback()
  {
    auto& state = registry();
    std::lock_guard<std::mutex> lock{state.mutex};
    if (!state.sink_callback) {
      throw std::logic_error("Process-B RecoArrays sink callback is not installed");
    }
    return state.sink_callback;
  }

  class RecoFrameProcessBSource : public WireCell::IFrameSource {
  public:
    bool operator()(WireCell::IFrame::pointer& output) override
    {
      auto& state = registry();
      std::lock_guard<std::mutex> lock{state.mutex};
      if (!state.prepared || state.taken) {
        throw std::logic_error("Process-B source used without a prepared event");
      }
      if (!emitted_) {
        output = state.frame;
        emitted_ = true;
        return true;
      }
      if (!sent_eos_) {
        output = nullptr;
        sent_eos_ = true;
        return true;
      }
      output = nullptr;
      return false;
    }

  private:
    bool emitted_ = false;
    bool sent_eos_ = false;
  };

  class MemoryMultiAlgBlobClusteringProxy : public WireCell::ITensorSetFilter,
                                             public WireCell::IConfigurable {
  public:
    WireCell::Configuration default_configuration() const override
    {
      WireCell::Configuration config;
      config["apa"] = -1;
      config["inpath"] = ".*";
      config["outpath"] = "";
      config["perf"] = false;
      config["dead_live_overlap_offset"] = 2;
      config["anode"] = "";
      config["face"] = 0;
      config["geom_helper"] = "";
      config["func_cfgs"] = Json::arrayValue;
      return config;
    }

    void configure(WireCell::Configuration const& config) override
    {
      config_ = config;
      auto const apa = WireCell::get<int>(config_, "apa", -1);
      if (apa < 0 || apa > 1) {
        throw std::invalid_argument("Process-B memory clustering proxy APA must be zero or one");
      }
    }

    bool operator()(input_pointer const& input, output_pointer& output) override
    {
      return clustering_callback()(config_, input, output);
    }

  private:
    WireCell::Configuration config_;
  };

  class RecoArraysProcessBSinkProxy : public WireCell::ITensorSetSink,
                                      public WireCell::IConfigurable {
  public:
    WireCell::Configuration default_configuration() const override
    {
      WireCell::Configuration config;
      config["apa"] = -1;
      config["datapath"] = "pointtrees/%d/live";
      config["anode"] = "";
      config["face"] = 0;
      config["geom_helper"] = "";
      return config;
    }

    void configure(WireCell::Configuration const& config) override
    {
      config_ = config;
      apa_ = WireCell::get<int>(config_, "apa", -1);
      if (apa_ < 0 || apa_ > 1) {
        throw std::invalid_argument("Process-B RecoArrays proxy APA must be zero or one");
      }
    }

    bool operator()(WireCell::ITensorSet::pointer const& input) override
    {
      return sink_callback()(apa_, config_, input);
    }

  private:
    int apa_ = -1;
    WireCell::Configuration config_;
  };

} // namespace

WIRECELL_FACTORY(wclsRecoFrameProcessBSource,
                 RecoFrameProcessBSource,
                 WireCell::IFrameSource)

WIRECELL_FACTORY(wclsMemoryMultiAlgBlobClustering,
                 MemoryMultiAlgBlobClusteringProxy,
                 WireCell::ITensorSetFilter,
                 WireCell::IConfigurable)

WIRECELL_FACTORY(wclsRecoArraysProcessBSink,
                 RecoArraysProcessBSinkProxy,
                 WireCell::ITensorSetSink,
                 WireCell::IConfigurable)

void wcls::install_process_b_callbacks(ProcessBMemoryClusteringCallback clustering,
                                       ProcessBRecoArraysSinkCallback sink)
{
  if (!clustering || !sink) {
    throw std::invalid_argument("Process-B callbacks must be non-empty");
  }
  auto& state = registry();
  std::lock_guard<std::mutex> lock{state.mutex};
  if (state.prepared && !state.taken) {
    throw std::logic_error("cannot replace Process-B callbacks during an active event");
  }
  state.clustering_callback = std::move(clustering);
  state.sink_callback = std::move(sink);
}

void wcls::prepare_process_b(RecoFramePayload const& payload,
                             std::string const& legacy_compare_directory)
{
  validate_reco_frame_payload(payload);
  auto& state = registry();
  std::lock_guard<std::mutex> lock{state.mutex};
  if (!state.clustering_callback || !state.sink_callback) {
    throw std::logic_error("Process-B callbacks must be installed before preparing an event");
  }
  if (state.prepared && !state.taken) {
    throw std::logic_error("Process-B already has an active event");
  }
  state.frame = make_reco_frame(payload);
  state.result = ProcessBResult{};
  state.result.identity = payload.identity;
  state.legacy_compare_directory = legacy_compare_directory;
  state.prepared = true;
  state.taken = false;
}

void wcls::record_process_b_memory_clustering(int const apa,
                                              std::int32_t const tensor_ident)
{
  if (apa < 0 || apa > 1) {
    throw std::invalid_argument("Process-B memory clustering APA must be zero or one");
  }
  auto& state = registry();
  std::lock_guard<std::mutex> lock{state.mutex};
  if (!state.prepared || state.taken) {
    throw std::logic_error("Process-B memory clustering ran without a prepared event");
  }
  auto& result = state.result.apa.at(static_cast<std::size_t>(apa));
  if (result.memory_clustering_completed) {
    throw std::logic_error("Process-B memory clustering ran more than once for one APA");
  }
  result.memory_clustering_completed = true;
  result.clustering_tensor_ident = tensor_ident;
}

std::string wcls::process_b_legacy_compare_directory()
{
  auto& state = registry();
  std::lock_guard<std::mutex> lock{state.mutex};
  if (!state.prepared || state.taken) {
    throw std::logic_error("Process-B legacy comparison queried without an active event");
  }
  return state.legacy_compare_directory;
}

void wcls::record_process_b_sink_eos(int const apa)
{
  if (apa < 0 || apa > 1) {
    throw std::invalid_argument("Process-B sink EOS APA must be zero or one");
  }
  auto& state = registry();
  std::lock_guard<std::mutex> lock{state.mutex};
  state.result.apa.at(static_cast<std::size_t>(apa)).saw_eos = true;
}

void wcls::store_process_b_reco_arrays(int const apa,
                                       std::int32_t const tensor_ident,
                                       RecoArrays arrays,
                                       bool const legacy_equivalent)
{
  if (apa < 0 || apa > 1) {
    throw std::invalid_argument("Process-B RecoArrays APA must be zero or one");
  }
  auto& state = registry();
  std::lock_guard<std::mutex> lock{state.mutex};
  if (!state.prepared || state.taken) {
    throw std::logic_error("Process-B sink stored arrays without an active event");
  }
  auto& result = state.result.apa.at(static_cast<std::size_t>(apa));
  if (result.received) {
    throw std::logic_error("Process-B sink received more than one tensor set");
  }
  result.received = true;
  result.legacy_equivalent = legacy_equivalent;
  result.tensor_ident = tensor_ident;
  result.reco_arrays = std::move(arrays);
}

wcls::ProcessBResult wcls::take_process_b_result()
{
  auto& state = registry();
  std::lock_guard<std::mutex> lock{state.mutex};
  if (!state.prepared || state.taken) {
    throw std::logic_error("Process-B has no completed event to take");
  }
  for (std::size_t apa = 0U; apa < state.result.apa.size(); ++apa) {
    auto const& result = state.result.apa[apa];
    if (!result.memory_clustering_completed || !result.received || !result.saw_eos ||
        result.reco_arrays.arrays.empty()) {
      throw std::runtime_error("Process-B did not complete both APA outputs");
    }
    if (result.clustering_tensor_ident != result.tensor_ident) {
      throw std::runtime_error("Process-B memory clustering and sink identities differ");
    }
  }
  if (state.result.apa[0].tensor_ident != state.result.apa[1].tensor_ident) {
    throw std::runtime_error("Process-B APA tensor identities differ");
  }
  state.taken = true;
  state.prepared = false;
  state.frame.reset();
  return std::move(state.result);
}
