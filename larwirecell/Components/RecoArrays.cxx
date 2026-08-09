#include "RecoArrays.h"

#include "WireCellClus/Facade_Blob.h"
#include "WireCellClus/Facade_Cluster.h"
#include "WireCellClus/Facade_Grouping.h"
#include "WireCellClus/Facade_Util.h"
#include "WireCellUtil/Exceptions.h"
#include "WireCellUtil/Stream.h"

#include <algorithm>
#include <boost/multi_array.hpp>
#include <cstring>
#include <limits>
#include <regex>
#include <stdexcept>
#include <unordered_map>

namespace {

  using WireCell::PointCloud::Facade::Blob;
  using WireCell::PointCloud::Facade::Grouping;
  using WireCell::PointCloud::Facade::MCUGraph;

  wcls::RecoArray& append(wcls::RecoArrays& output,
                          std::string const& name,
                          std::size_t const rows,
                          std::size_t const columns)
  {
    if (columns != 0U && rows > std::numeric_limits<std::size_t>::max() / columns) {
      throw std::length_error("RecoArray shape overflows size_t");
    }
    output.arrays.push_back(wcls::RecoArray{name, rows, columns, {}});
    auto& result = output.arrays.back();
    result.values.resize(rows * columns);
    return result;
  }

  float& at(wcls::RecoArray& array, std::size_t const row, std::size_t const column)
  {
    return array.values.at(row * array.columns + column);
  }

} // namespace

wcls::RecoArrays wcls::make_reco_arrays(Grouping const& grouping)
{
  RecoArrays result;
  using HistoricalArray = boost::multi_array<float, 2U>;

  auto const& grouping_point_clouds = grouping.node()->value.local_pcs();
  std::regex const ctpc_name_pattern{"^ctpc_f([0-9]+)p([0-9]+)$"};

  std::size_t blob_count = 0U;
  for (auto const* cluster : grouping.children()) {
    blob_count += cluster->children().size();
  }
  // Preserve the literal legacy representation, including untouched corner
  // padding after ncorners.  grouping2file used boost::multi_array<float>
  // storage and serialized all 38 columns without initializing that padding.
  HistoricalArray historical_blobs(boost::extents[blob_count][38U]);
  std::size_t global_blob_index = 0U;
  std::unordered_map<Blob const*, int> blob_to_index;
  for (auto const* cluster : grouping.children()) {
    for (auto const* blob : cluster->children()) {
      historical_blobs[global_blob_index][0U] = blob->charge();
      auto const corner_count = std::min(12, static_cast<int>(blob->corners().size()));
      historical_blobs[global_blob_index][1U] = corner_count;
      for (int corner = 0; corner < corner_count; ++corner) {
        historical_blobs[global_blob_index][2U + static_cast<std::size_t>(corner) * 3U] =
          blob->corners().at(corner).x();
        historical_blobs[global_blob_index][3U + static_cast<std::size_t>(corner) * 3U] =
          blob->corners().at(corner).y();
        historical_blobs[global_blob_index][4U + static_cast<std::size_t>(corner) * 3U] =
          blob->corners().at(corner).z();
      }
      if (global_blob_index > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("RecoArrays blob index exceeds historical int range");
      }
      blob_to_index[blob] = static_cast<int>(global_blob_index);
      ++global_blob_index;
    }
  }

  // Emit the CTPC arrays in their original order after the legacy-sized blob
  // staging allocation has captured its untouched padding bytes.
  for (auto const& [name, point_cloud] : grouping_point_clouds) {
    std::smatch matches;
    if (!std::regex_match(name, matches, ctpc_name_pattern)) {
      continue;
    }
    auto const points = point_cloud.size_major();
    auto& ctpc = append(result, name, points, 7U);
    auto const& x = point_cloud.get("x")->elements<double>();
    auto const& y = point_cloud.get("y")->elements<double>();
    auto const& charge = point_cloud.get("charge")->elements<double>();
    auto const& charge_error = point_cloud.get("charge_err")->elements<double>();
    auto const& channel_ident = point_cloud.get("cident")->elements<int>();
    auto const& wire_index = point_cloud.get("wind")->elements<int>();
    auto const& slice_index = point_cloud.get("slice_index")->elements<int>();
    for (std::size_t index = 0U; index < points; ++index) {
      at(ctpc, index, 0U) = x[index];
      at(ctpc, index, 1U) = y[index];
      at(ctpc, index, 2U) = charge[index];
      at(ctpc, index, 3U) = charge_error[index];
      at(ctpc, index, 4U) = channel_ident[index];
      at(ctpc, index, 5U) = wire_index[index];
      at(ctpc, index, 6U) = slice_index[index];
    }
  }
  auto& blobs = append(result, "blobs", blob_count, 38U);
  if (!blobs.values.empty()) {
    std::memcpy(blobs.values.data(), historical_blobs.data(),
                blobs.values.size() * sizeof(float));
  }

  std::size_t point_count = 0U;
  std::size_t edge_count = 0U;
  for (auto const* cluster : grouping.children()) {
    cluster->Create_graph(true);
    auto const& graph = *cluster->get_graph();
    point_count += boost::num_vertices(graph);
    edge_count += boost::num_edges(graph);
  }

  auto& points = append(result, "points", point_count, 6U);
  auto& edges = append(result, "ppedges", edge_count, 3U);

  std::size_t global_cluster_index = 0U;
  std::size_t global_edge_index = 0U;
  std::size_t global_point_offset = 0U;
  for (auto const* cluster : grouping.children()) {
    MCUGraph const& graph = *cluster->get_graph();
    auto const vertices = boost::vertices(graph);
    for (auto iterator = vertices.first; iterator != vertices.second; ++iterator) {
      auto const vertex = *iterator;
      auto const global_point_index = global_point_offset + vertex;
      if (global_point_index >= point_count) {
        throw std::out_of_range("RecoArrays global point index exceeds point count");
      }
      auto const point = cluster->point3d(graph[vertex].index);
      at(points, global_point_index, 0U) = point.x();
      at(points, global_point_index, 1U) = point.y();
      at(points, global_point_index, 2U) = point.z();
      auto const closest = cluster->get_closest_point_blob(
        {at(points, global_point_index, 0U),
         at(points, global_point_index, 1U),
         at(points, global_point_index, 2U)});
      auto const* blob = closest.second;
      at(points, global_point_index, 3U) = blob->charge() / blob->npoints();
      at(points, global_point_index, 4U) = blob_to_index.at(blob);
      at(points, global_point_index, 5U) = global_cluster_index;
    }

    auto const graph_edges = boost::edges(graph);
    for (auto iterator = graph_edges.first; iterator != graph_edges.second; ++iterator) {
      auto const edge = *iterator;
      at(edges, global_edge_index, 0U) = boost::source(edge, graph) + global_point_offset;
      at(edges, global_edge_index, 1U) = boost::target(edge, graph) + global_point_offset;
      at(edges, global_edge_index, 2U) = boost::get(boost::edge_weight, graph, edge);
      ++global_edge_index;
    }
    global_point_offset += boost::num_vertices(graph);
    ++global_cluster_index;
  }

  return result;
}

void wcls::write_historical_reco_arrays(Grouping const& reference,
                                        std::string const& legacy_npz_path)
{
  WireCell::PointCloud::Facade::grouping2file(reference, legacy_npz_path);
}

void wcls::compare_historical_reco_arrays(RecoArrays const& candidate,
                                          std::string const& legacy_npz_path)
{
  boost::iostreams::filtering_istream input;
  WireCell::Stream::input_filters(input, legacy_npz_path);
  if (input.empty()) {
    throw std::runtime_error("failed to open historical grouping2file NPZ");
  }

  for (auto const& expected : candidate.arrays) {
    std::string name;
    std::size_t file_size = 0U;
    custard::read(input, name, file_size);
    pigenc::File serialized;
    serialized.read(input);
    if (!input) {
      throw std::runtime_error("historical grouping2file NPZ ended before all candidate arrays");
    }
    auto const& header = serialized.header();
    if (name != expected.name) {
      throw std::runtime_error("historical grouping2file key order differs at " + expected.name);
    }
    if (header.shape().size() != 2U || header.shape()[0] != expected.rows ||
        header.shape()[1] != expected.columns) {
      throw std::runtime_error("historical grouping2file shape differs for " + expected.name);
    }
    if (header.dtype() != "f4" || header.fortran_order()) {
      throw std::runtime_error("historical grouping2file dtype/order differs for " + expected.name);
    }
    auto const bytes = serialized.data();
    auto const expected_bytes = expected.values.size() * sizeof(float);
    if (file_size != header.file_size() || bytes.size() != expected_bytes) {
      throw std::runtime_error("historical grouping2file byte count differs for " + expected.name);
    }
    if (expected_bytes != 0U &&
        std::memcmp(bytes.data(), expected.values.data(), expected_bytes) != 0) {
      auto const* actual = reinterpret_cast<unsigned char const*>(bytes.data());
      auto const* wanted = reinterpret_cast<unsigned char const*>(expected.values.data());
      auto const mismatch = std::mismatch(actual, actual + expected_bytes, wanted);
      auto const byte_index = static_cast<std::size_t>(mismatch.first - actual);
      auto const element_index = byte_index / sizeof(float);
      float actual_value = 0.0F;
      float expected_value = 0.0F;
      std::memcpy(&actual_value, bytes.data() + element_index * sizeof(float), sizeof(float));
      std::memcpy(&expected_value, expected.values.data() + element_index, sizeof(float));
      auto const row = element_index / expected.columns;
      float actual_count = 0.0F;
      float expected_count = 0.0F;
      if (expected.name == "blobs" && expected.columns == 38U) {
        auto const count_index = row * expected.columns + 1U;
        std::memcpy(&actual_count, bytes.data() + count_index * sizeof(float), sizeof(float));
        std::memcpy(&expected_count, expected.values.data() + count_index, sizeof(float));
      }
      throw std::runtime_error("historical grouping2file raw bytes differ for " + expected.name +
                               " at byte " + std::to_string(byte_index) + " (element " +
                               std::to_string(element_index) + ", historical " +
                               std::to_string(actual_value) + ", external " +
                               std::to_string(expected_value) + ", row " + std::to_string(row) +
                               " counts " + std::to_string(actual_count) + "/" +
                               std::to_string(expected_count) + ")");
    }
  }
  input.pop();
}
