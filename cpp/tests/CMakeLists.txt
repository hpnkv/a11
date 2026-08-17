add_executable(a11_core_test
  byte_chunking_test.cc
  data_json_test.cc
  data_types_test.cc
  future_test.cc
  http1_codec_test.cc
  http1_connection_test.cc
  http2_test.cc
  http_url_test.cc
  http_fetch_test.cc
  http_connection_pool_test.cc
  http_download_test.cc
  http_sse_wire_stream_test.cc
  signalling_test.cc
  webrtc_wire_stream_test.cc
  websocket_signalling_test.cc
  chunk_store_reader_test.cc
  chunk_store_writer_test.cc
  async_node_test.cc
  action_test.cc
  exception_boundary_test.cc
  service_test.cc
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

# The Flow language. Its own executable because a11::flow_lang links nothing but
# Abseil and nlohmann, and a test binary that proves it stays that way is worth
# more than one more file in a11_core_test.
add_executable(a11_flow_test
  flow_complete_test.cc
  flow_diagnostic_test.cc
  flow_dto_test.cc
  flow_emit_json_test.cc
  flow_highlight_test.cc
  flow_inspect_test.cc
  flow_format_test.cc
  flow_lexer_test.cc
  flow_navigate_test.cc
  flow_offsets_test.cc
  flow_parser_test.cc
  flow_pattern_test.cc
  flow_resolve_test.cc)
target_link_libraries(a11_flow_test PRIVATE a11::flow_lang GTest::gtest_main)
add_test(NAME a11_flow_test COMMAND a11_flow_test)

# The executing half, which does link the node and action layers. Kept out of
# a11_flow_test on purpose: that binary's link line is the proof the language
# needs nothing but Abseil and nlohmann.
add_executable(a11_flow_runtime_test
  flow_runtime_test.cc
  flow_shape_test.cc
  flow_values_test.cc)
target_link_libraries(
  a11_flow_runtime_test PRIVATE a11::flow_runtime GTest::gtest_main)
add_test(NAME a11_flow_runtime_test COMMAND a11_flow_runtime_test)

if (A11_BUILD_REDIS)
  add_executable(a11_redis_test
    redis_client_test.cc
    redis_chunk_store_test.cc)
  target_link_libraries(
    a11_redis_test PRIVATE a11::concurrency a11::data a11::redis a11::stores
                           GTest::gtest_main)
  add_test(NAME a11_redis_test COMMAND a11_redis_test)
endif ()

if (A11_BUILD_SQLITE)
  add_executable(a11_sqlite_test
    sqlite_chunk_store_test.cc)
  target_include_directories(a11_sqlite_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(
    a11_sqlite_test PRIVATE a11::concurrency a11::data a11::stores
                            GTest::gtest_main)
  add_test(NAME a11_sqlite_test COMMAND a11_sqlite_test)
endif ()

if (A11_BUILD_HTTP)
  # The two HTTP Actions, against a real Http2Server. Its own executable because
  # a11::http_actions is the only thing under test and a focused link line is
  # what proves that.
  add_executable(a11_http_actions_test
    http_actions_test.cc)
  target_link_libraries(
    a11_http_actions_test PRIVATE a11::http_actions a11::actions a11::concurrency
                                  a11::data a11::net a11::nodes a11::stores
                                  GTest::gtest_main)
  add_test(NAME a11_http_actions_test COMMAND a11_http_actions_test)
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
    audio_actions_test.cc
    audio_model_registry_test.cc
    serial_tags_test.cc)
  target_link_libraries(
    a11_audio_actions_test PRIVATE a11::audio_actions a11::actions a11::audio
                                   a11::concurrency a11::data a11::net
                                   a11::nodes GTest::gtest_main)
  add_test(NAME a11_audio_actions_test COMMAND a11_audio_actions_test)
endif ()
