#ifndef LARWIRECELL_COMPONENTS_RECOARRAYS_H
#define LARWIRECELL_COMPONENTS_RECOARRAYS_H

#include <cstddef>
#include <string>
#include <vector>

namespace WireCell::PointCloud::Facade {
  class Grouping;
}

namespace wcls {

  // External, owning representation of the historical grouping2file arrays.
  // Values are row-major IEEE float32 and entries remain in serializer order.
  struct RecoArray {
    std::string name;
    std::size_t rows = 0U;
    std::size_t columns = 0U;
    std::vector<float> values;
  };

  struct RecoArrays {
    std::vector<RecoArray> arrays;
  };

  // This is the accepted 01dc76b0 six-array construction body moved outside
  // libWireCellClus.  It deliberately defines no PointCloud::Facade symbols.
  RecoArrays make_reco_arrays(WireCell::PointCloud::Facade::Grouping const& grouping);

  // Test-only boundary comparison.  Write the independently recreated
  // reference Grouping through grouping2file() exported by the loaded
  // historical Clus ELF, then compare that NPZ in order against the candidate
  // arrays, including f4 dtype, shape and raw bytes.
  void write_historical_reco_arrays(WireCell::PointCloud::Facade::Grouping const& reference,
                                    std::string const& legacy_npz_path);
  void compare_historical_reco_arrays(RecoArrays const& candidate,
                                      std::string const& legacy_npz_path);

} // namespace wcls

#endif
