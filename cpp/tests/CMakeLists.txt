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

if (A11_BUILD_REDIS)
  add_executable(a11_redis_test
    redis_client_test.cc
    redis_chunk_store_test.cc)
  target_link_libraries(
    a11_redis_test PRIVATE a11::concurrency a11::data a11::redis a11::stores
                           GTest::gtest_main)
  add_test(NAME a11_redis_test COMMAND a11_redis_test)
endif ()

if (A11_BUILD_AUDIO)
  add_executable(a11_audio_test
    audio_buffer_test.cc
    audio_input_test.cc
    audio_processing_test.cc
    sample_ring_test.cc
    speech_recognizer_test.cc)
  target_link_libraries(
    a11_audio_test PRIVATE a11::audio a11::concurrency GTest::gtest_main)
  add_test(NAME a11_audio_test COMMAND a11_audio_test)

  add_executable(a11_audio_actions_test
    audio_serialization_test.cc
    audio_actions_test.cc)
  target_link_libraries(
    a11_audio_actions_test PRIVATE a11::audio_actions a11::actions a11::audio
                                   a11::concurrency a11::data a11::nodes
                                   GTest::gtest_main)
  add_test(NAME a11_audio_actions_test COMMAND a11_audio_actions_test)
endif ()
