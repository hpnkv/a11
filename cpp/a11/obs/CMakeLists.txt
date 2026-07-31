set(A11_OBS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/provider.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/trace_context.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/tracer.cc)
if (A11_WITH_OTLP_HTTP)
    list(APPEND A11_OBS_SOURCES
         ${CMAKE_CURRENT_SOURCE_DIR}/internal/otlp_http_exporter.cc)
endif ()
set(A11_OBS_SOURCES ${A11_OBS_SOURCES} PARENT_SCOPE)
