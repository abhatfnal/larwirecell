cet_build_plugin(BlipMaker art::EDProducer
  LIBRARIES PRIVATE
  lardataobj::Simulation)

cet_build_plugin(EventButcher art::EDProducer
  LIBRARIES PRIVATE
  lardataobj::RecoBase
  lardataobj::RawData
  canvas::canvas
  art::Persistency_Common
  )

cet_build_plugin(RecoFramePayloadEquivalence art::EDProducer
  LIBRARIES PRIVATE
  larwirecell::WireCellLarsoft
  art::Framework_Core
  art::Framework_Principal
  cetlib_except::cetlib_except
  WireCell::Iface
  WireCell::Util
)

cet_build_plugin(WireCellToolkit art::SharedProducer
  LIBRARIES PRIVATE
  art::Utilities
  )

add_subdirectory(Geometry)

install_headers()
install_source()
