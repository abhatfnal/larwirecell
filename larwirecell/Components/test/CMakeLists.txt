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

cet_test(reco_arrays_protocol_test
  SOURCE RecoArraysProtocol_test.cxx
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

cet_test(reco_frame_process_b_writer_policy_test
  SOURCE RecoFrameProcessBWriterPolicy_test.cxx
  TEST_ARGS
  ${CMAKE_CURRENT_SOURCE_DIR}/../../wctcfg/reco-frame-process-b.jsonnet
  ${CMAKE_CURRENT_SOURCE_DIR}/../RecoFrameProcessBWorker.cxx
  ${CMAKE_CURRENT_SOURCE_DIR}/../MemoryMultiAlgBlobClustering.cxx
  ${CMAKE_CURRENT_SOURCE_DIR}/../RecoFrameProcessBComponents.cxx
  ${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt
  ${CMAKE_CURRENT_SOURCE_DIR}/../process_c_python311_launcher.sh
)

cet_test(reco_frame_process_b_parent_mode_protocol_test
  SOURCE RecoFrameProcessBParentModeProtocol_test.cxx
  TEST_ARGS $<TARGET_FILE:reco_frame_process_b_worker>
  LIBRARIES
  larwirecell::WireCellLarsoft
)
add_dependencies(reco_frame_process_b_parent_mode_protocol_test reco_frame_process_b_worker)
