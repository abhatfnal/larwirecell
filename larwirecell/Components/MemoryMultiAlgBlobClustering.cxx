#include "RecoFrameProcessB.h"

#include "WireCellAux/SimpleTensorSet.h"
#include "WireCellAux/TensorDMpointtree.h"
#include "WireCellClus/ClusteringFuncs.h"
#include "WireCellClus/Facade.h"
#include "WireCellClus/IClusGeomHelper.h"
#include "WireCellIface/IAnodePlane.h"
#include "WireCellUtil/Exceptions.h"
#include "WireCellUtil/ExecMon.h"
#include "WireCellUtil/Logging.h"
#include "WireCellUtil/NamedFactory.h"
#include "WireCellUtil/String.h"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

  using WireCell::PointCloud::Facade::Grouping;

  // Preserve the archived driver's performance/logging touch points without
  // carrying over any of its Bee or file-output diagnostics.
  struct PerformanceLog {
    bool enabled;
    WireCell::Log::logptr_t log;
    WireCell::ExecMon monitor;

    PerformanceLog(bool const enable,
                   WireCell::Log::logptr_t logger,
                   std::string const& title)
      : enabled{enable}, log{std::move(logger)}, monitor{title}
    {}

    ~PerformanceLog()
    {
      if (enabled) {
        log->debug("MemoryMultiAlgBlobClustering performance summary:\n{}",
                   monitor.summary());
      }
    }

    void mark(std::string const& context)
    {
      if (enabled) {
        monitor(context);
      }
    }

    void grouping(std::string const& context, Grouping const& value)
    {
      if (!enabled) {
        return;
      }
      mark(context);
      log->debug("{} grouping {}", context, value);
    }
  };

  class MemoryMultiAlgBlobClustering {
  public:
    bool operator()(WireCell::Configuration const& config,
                    WireCell::ITensorSet::pointer const& input,
                    WireCell::ITensorSet::pointer& output)
    {
      output = nullptr;
      if (!input) {
        log_->debug("EOS at call {}", call_count_++);
        return true;
      }

      auto const apa = WireCell::get<int>(config, "apa", -1);
      auto const face = WireCell::get<int>(config, "face", 0);
      auto input_path = WireCell::get<std::string>(config, "inpath", ".*");
      auto output_path = WireCell::get<std::string>(config, "outpath", "");
      auto const performance_enabled = WireCell::get<bool>(config, "perf", false);
      auto const& function_configurations = config["func_cfgs"];
      if (apa < 0 || apa > 1) {
        throw std::invalid_argument("memory clustering APA must be zero or one");
      }
      if (!function_configurations.isArray() || function_configurations.empty()) {
        throw std::invalid_argument("memory clustering requires an ordered func_cfgs array");
      }
      auto const anode = WireCell::Factory::find_tn<WireCell::IAnodePlane>(
        config["anode"].asString());
      auto const geometry_helper = WireCell::Factory::find_tn<WireCell::IClusGeomHelper>(
        config["geom_helper"].asString());

      PerformanceLog performance{
        performance_enabled, log_, "starting MemoryMultiAlgBlobClustering"};
      auto const ident = input->ident();
      log_->debug("loading tensor set ident={}", ident);

      if (input_path.find('%') != std::string::npos) {
        input_path = WireCell::String::format(input_path, ident);
      }
      auto const& tensors = *input->tensors();
      auto live_root = WireCell::Aux::TensorDM::as_pctree(tensors, input_path + "/live");
      if (!live_root) {
        throw std::runtime_error("memory clustering could not recover the live point tree");
      }
      auto* live_grouping = live_root->value.facade<Grouping>();
      live_grouping->set_anode(anode);
      live_grouping->set_params(geometry_helper->get_params(anode->ident(), face));
      performance.mark("loaded live clusters");

      auto const dead_path = input_path + "/dead";
      WireCell::PointCloud::Tree::Points::node_ptr dead_root;
      try {
        dead_root = WireCell::Aux::TensorDM::as_pctree(tensors, dead_path);
        performance.mark("loaded dead clusters");
      }
      catch (WireCell::KeyError const&) {
        log_->warn("no point tree at {}, assuming no dead clusters", dead_path);
        dead_root = std::make_unique<WireCell::PointCloud::Tree::Points::node_t>();
      }
      auto* dead_grouping = dead_root->value.facade<Grouping>();
      dead_grouping->set_anode(anode);
      dead_grouping->set_params(geometry_helper->get_params(anode->ident(), face));

      WireCell::PointCloud::Facade::cluster_set_t cluster_connected_dead;
      performance.grouping("pre clustering", *live_grouping);

      // Preserve the archived driver's cache-touching calls before visitors.
      auto& dead_u_index = live_grouping->get_dead_winds(0, 0);
      auto& dead_v_index = live_grouping->get_dead_winds(0, 1);
      auto& dead_w_index = live_grouping->get_dead_winds(0, 2);
      log_->debug("dead_u_index size {}", dead_u_index.size());
      log_->debug("dead_v_index size {}", dead_v_index.size());
      log_->debug("dead_w_index size {}", dead_w_index.size());

      for (auto const& function_configuration : function_configurations) {
        auto function =
          WireCell::PointCloud::Facade::getClusteringFunction(function_configuration);
        function(*live_grouping, *dead_grouping, cluster_connected_dead);
        performance.grouping(function_configuration["name"].asString(), *live_grouping);
      }

      if (output_path.find('%') != std::string::npos) {
        output_path = WireCell::String::format(output_path, ident);
      }
      auto output_tensors =
        WireCell::Aux::TensorDM::as_tensors(*live_root, output_path + "/live");
      performance.mark("output live clusters to tensors");
      auto dead_tensors =
        WireCell::Aux::TensorDM::as_tensors(*dead_root, output_path + "/dead");
      performance.mark("output dead clusters to tensors");
      output_tensors.insert(output_tensors.end(), dead_tensors.begin(), dead_tensors.end());
      output = WireCell::Aux::TensorDM::as_tensorset(output_tensors, ident);
      performance.mark("combine tensors");

      wcls::record_process_b_memory_clustering(apa, ident);
      live_root.reset();
      dead_root.reset();
      performance.mark("clear pc tree memory");
      return true;
    }

  private:
    WireCell::Log::logptr_t log_ = WireCell::Log::logger("clus");
    std::size_t call_count_ = 0U;
  };

} // namespace

bool wcls::run_process_b_memory_clustering(WireCell::Configuration const& config,
                                           WireCell::ITensorSet::pointer const& input,
                                           WireCell::ITensorSet::pointer& output)
{
  static MemoryMultiAlgBlobClustering algorithm;
  return algorithm(config, input, output);
}
