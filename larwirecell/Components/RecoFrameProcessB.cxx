#include "RecoFrameProcessB.h"

#include "WireCellAux/TensorDMpointtree.h"
#include "WireCellClus/Facade_Grouping.h"
#include "WireCellClus/IClusGeomHelper.h"
#include "WireCellIface/IAnodePlane.h"
#include "WireCellUtil/NamedFactory.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

bool wcls::consume_process_b_reco_arrays(
  int const apa,
  WireCell::Configuration const& config,
  WireCell::ITensorSet::pointer const& input)
{
  if (!input) {
    record_process_b_sink_eos(apa);
    return true;
  }

  auto path = WireCell::get<std::string>(config, "datapath", "pointtrees/%d/live");
  auto const marker = path.find("%d");
  if (marker != std::string::npos) {
    path.replace(marker, 2U, std::to_string(input->ident()));
  }
  auto const anode = WireCell::Factory::find_tn<WireCell::IAnodePlane>(
    config["anode"].asString());
  auto const geometry_helper = WireCell::Factory::find_tn<WireCell::IClusGeomHelper>(
    config["geom_helper"].asString());
  auto const face = WireCell::get<int>(config, "face", 0);
  auto const& tensors = *input->tensors();

  auto candidate_tree = WireCell::Aux::TensorDM::as_pctree(tensors, path);
  if (!candidate_tree) {
    throw std::runtime_error("Process-B sink could not recover final post-clustering pctree");
  }
  auto* candidate_grouping =
    candidate_tree->value.facade<WireCell::PointCloud::Facade::Grouping>();
  if (!candidate_grouping) {
    throw std::runtime_error("Process-B sink could not recover final post-clustering Grouping");
  }
  candidate_grouping->set_anode(anode);
  candidate_grouping->set_params(geometry_helper->get_params(anode->ident(), face));

  auto const legacy_directory = process_b_legacy_compare_directory();
  RecoArrays arrays;
  bool equivalent = false;
  if (!legacy_directory.empty()) {
    // Recreate an independent facade from this exact final tensor state for
    // the explicitly test-only historical boundary comparison.
    auto reference_tree = WireCell::Aux::TensorDM::as_pctree(tensors, path);
    if (!reference_tree) {
      throw std::runtime_error("Process-B sink could not recreate reference pctree");
    }
    auto* reference_grouping =
      reference_tree->value.facade<WireCell::PointCloud::Facade::Grouping>();
    if (!reference_grouping) {
      throw std::runtime_error("Process-B sink could not recreate reference Grouping");
    }
    reference_grouping->set_anode(anode);
    reference_grouping->set_params(geometry_helper->get_params(anode->ident(), face));
    auto const legacy_path = std::filesystem::path{legacy_directory} /
                             ("legacy-boundary-apa" + std::to_string(apa) + ".npz");
    write_historical_reco_arrays(*reference_grouping, legacy_path.string());
    arrays = make_reco_arrays(*candidate_grouping);
    compare_historical_reco_arrays(arrays, legacy_path.string());
    equivalent = true;
  }
  else {
    arrays = make_reco_arrays(*candidate_grouping);
  }

  store_process_b_reco_arrays(apa, input->ident(), std::move(arrays), equivalent);
  return true;
}

void wcls::install_process_b_runtime()
{
  install_process_b_callbacks(run_process_b_memory_clustering,
                              consume_process_b_reco_arrays);
}
