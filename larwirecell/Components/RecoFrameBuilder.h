#ifndef LARWIRECELL_COMPONENTS_RECOFRAMEBUILDER_H
#define LARWIRECELL_COMPONENTS_RECOFRAMEBUILDER_H

#include "RecoFramePayload.h"

#include "WireCellIface/IFrame.h"

#include <deque>

namespace wcls {

  WireCell::IFrame::pointer make_reco_frame(RecoFramePayload const& payload);

  // Minimal in-process source adapter matching CookedFrameSource's observable
  // contract: one frame, one successful null/EOS item, then exhaustion.
  class RecoFramePayloadSource {
  public:
    explicit RecoFramePayloadSource(RecoFramePayload const& payload);

    bool operator()(WireCell::IFrame::pointer& frame);

  private:
    std::deque<WireCell::IFrame::pointer> frames_;
  };

} // namespace wcls

#endif
