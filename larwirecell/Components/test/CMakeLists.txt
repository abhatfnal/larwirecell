include(CetTest)

cet_test(reco_frame_payload_test
  SOURCE RecoFramePayload_test.cxx
  LIBRARIES
  larwirecell::WireCellLarsoft
  lardataobj::RecoBase
  WireCell::Iface
)

cet_test(reco_frame_protocol_test
  SOURCE RecoFrameProtocol_test.cxx
  LIBRARIES
  larwirecell::WireCellLarsoft
)

cet_test(reco_frame_ipc_worker_failure_test
  SOURCE RecoFrameIpcWorkerFailure_test.cxx
  TEST_ARGS $<TARGET_FILE:reco_frame_ipc_worker>
  LIBRARIES
  larwirecell::WireCellLarsoft
)
add_dependencies(reco_frame_ipc_worker_failure_test reco_frame_ipc_worker)
