# Build the libWireCellLarsoft WCT plugin library.
# See README for some details.

# This is what's needed for building the WireCellLarsoft WCT component
# plugin library.
cet_make_library(LIBRARY_NAME WireCellLarsoft
  SOURCE
  ChannelNoiseDB.cxx
  ChannelSelectorDB.cxx
  CookedFrameSink.cxx
  CookedFrameSource.cxx
  DepoFluxWriter.cxx
  FrameSaver.cxx
  LazyFrameSource.cxx
  MultiChannelNoiseDB.cxx
  OpFlashSource.cxx
  RawFrameSource.cxx
  RecoFrameBuilder.cxx
  RecoFrameExtractor.cxx
  RecoFrameIpcTestSupport.cxx
  RecoFramePayload.cxx
  RecoFrameProcessBComponents.cxx
  RecoFrameProtocol.cxx
  SimDepoSetSource.cxx
  SimDepoSource.cxx
  # vvv obsolete vvv
  DepoSetSimChannelSink.cxx
  SimChannelSink.cxx
  # ^^^ obsolete ^^^
  LIBRARIES
  PUBLIC
  larwirecell::IArtEventVisitor
  larcoreobj::headers
  canvas::canvas
  WireCell::SigProc
  WireCell::Iface
  WireCell::Util
  lardataobj::Simulation
  PRIVATE
  larevt::CalibrationDBI_IOVData
  larevt::ChannelStatusService
  larevt::ChannelStatusProvider
  larevt::DetPedestalService
  larevt::DetPedestalProvider
  larevt::ElectronicsCalibService
  larevt::ElectronicsCalibProvider
  lardata::DetectorPropertiesService
  larcore::Geometry_Geometry_service
  larcore::ServiceUtil
  lardataobj::RecoBase
  lardataobj::RawData
  art::Framework_Core
  art::Framework_Principal
  WireCell::Gen
  ROOT::Core
)

# WireCell's installed custard_boost.hpp includes miniz.h, but the v0.30.6
# product installs that private header only in its source payload.  It is needed
# solely by the Process-B executable's test-only historical-NPZ boundary reader.
file(GLOB _wirecell_miniz_headers
  "${WireCell_INCLUDE_DIR}/../../source/*/util/src/miniz.h")
if(NOT _wirecell_miniz_headers)
  message(FATAL_ERROR "WireCell miniz.h is required for the Process-B boundary test")
endif()
list(GET _wirecell_miniz_headers 0 _wirecell_miniz_header)
get_filename_component(_wirecell_miniz_include "${_wirecell_miniz_header}" DIRECTORY)

cet_make_exec(NAME reco_frame_ipc_worker
  SOURCE RecoFrameIpcWorker.cxx
  LIBRARIES larwirecell::WireCellLarsoft
)

cet_make_exec(NAME reco_frame_process_b_worker
  SOURCE RecoFrameProcessBWorker.cxx RecoFrameProcessB.cxx RecoArrays.cxx MemoryMultiAlgBlobClustering.cxx
  LIBRARIES
  larwirecell::WireCellLarsoft
  WireCell::Apps
  WireCell::Aux
  WireCell::Clus
)

install(PROGRAMS process_c_python311_launcher.sh DESTINATION bin)
target_include_directories(reco_frame_process_b_worker PRIVATE "${_wirecell_miniz_include}")

add_subdirectory(test)
