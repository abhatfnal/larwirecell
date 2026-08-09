#ifndef LARWIRECELL_COMPONENTS_RECOFRAMEEXTRACTOR_H
#define LARWIRECELL_COMPONENTS_RECOFRAMEEXTRACTOR_H

#include "RecoFramePayload.h"

#include <cstdint>
#include <string>
#include <vector>

namespace art {
  class Event;
}

namespace recob {
  class Wire;
}

namespace wcls {

  struct RecoFrameExtractConfig {
    std::uint32_t nticks = 3400U;
    double tick_period_ns = 500.0;
    double frame_scale = 50.0;
    double summary_scale = 50.0;
    std::vector<std::string> frame_tags{"orig"};
    std::vector<std::string> recobwire_tags{"simtpc2d:gauss", "simtpc2d:wiener"};
    std::vector<std::string> trace_tags{"gauss", "wiener"};
    std::vector<std::string> summary_tags{"", "simtpc2d:wienersummary"};
    std::vector<std::string> input_mask_tags{"simtpc2d:badmasks"};
    std::vector<std::string> output_mask_tags{"bad"};
  };

  RecoFramePayload extract_reco_frame_payload(art::Event const& event,
                                               RecoFrameExtractConfig const& config = {});

  namespace detail {

    // Test seam for proving that direct sparse-ROI extraction is bitwise
    // equivalent to recob::Wire::Signal() followed by CookedFrameSource's
    // dense scaling loop.  Production extraction uses this same function.
    SparseTraceSet extract_sparse_trace_set(std::vector<recob::Wire> const& wires,
                                            std::string const& trace_tag,
                                            std::uint32_t nticks,
                                            double frame_scale,
                                            std::vector<double> const& summary = {},
                                            double summary_scale = 1.0);

  } // namespace detail

} // namespace wcls

#endif
