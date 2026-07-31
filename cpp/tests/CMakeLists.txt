add_executable(a11_core_test
  byte_chunking_test.cc
  data_json_test.cc
  data_types_test.cc
  future_test.cc
  http2_test.cc
  http_sse_wire_stream_test.cc
  signalling_test.cc
  webrtc_wire_stream_test.cc
  websocket_signalling_test.cc
  chunk_store_reader_test.cc
  chunk_store_writer_test.cc
  async_node_test.cc
  action_test.cc
  session_test.cc
  in_process_wire_stream_test.cc
  wire_stream_with_recv_test.cc
  websocket_wire_stream_test.cc
  local_chunk_store_test.cc
  obs_trace_context_test.cc
  obs_tracer_test.cc
  obs_action_span_test.cc
  obs_session_stream_test.cc)
target_link_libraries(
  a11_core_test PRIVATE a11::concurrency a11::data a11::net a11::nodes
                        a11::stores a11::actions a11::service a11::obs
                        GTest::gtest_main)
add_test(NAME a11_core_test COMMAND a11_core_test)
