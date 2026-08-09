include(CetTest)

cet_test(reco_frame_payload_test
  SOURCE RecoFramePayload_test.cxx
  LIBRARIES
  larwirecell::WireCellLarsoft
  lardataobj::RecoBase
  WireCell::Iface
)
