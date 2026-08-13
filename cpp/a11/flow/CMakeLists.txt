# The Flow language, as a component that depends on nothing but the standard
# library, Abseil and nlohmann.
#
# That is deliberate and load-bearing: the lexer, parser, resolver, inspector,
# formatter and highlighter are what editors and CI need, and keeping them clear
# of OpenSSL, libuv, nghttp2, libdatachannel and PortAudio is what lets the
# tooling ship as a small binary on platforms where the full runtime is not
# built. The executing half of the language lives in A11_FLOW_RUNTIME_SOURCES,
# which does link the node and action layers.
set(A11_FLOW_LANG_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/catalogue.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/complete.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostic.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/emit_json.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/format.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/generate.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/graph.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/highlight.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/inspect.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/lexer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/navigate.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/offsets.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/parser.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/pattern.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/resolve.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/syntax.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/vocabulary.cc
    PARENT_SCOPE)

# The executing half: the value model and the runtime that drives A11's nodes
# and actions with it. Separate from the language because it is the only part
# that needs a node, a session or a socket -- an editor wants everything above
# and none of this, and keeping the two apart is what lets the tooling binary
# stay small enough to bundle per platform.
set(A11_FLOW_RUNTIME_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/values.cc
    PARENT_SCOPE)

# The standalone tool: the language, a command line and two protocols, linking
# nothing the library above does not. This is what an editor that cannot embed
# Python runs, and what the IntelliJ plugin bundles instead of carrying a lexer,
# a parser and an inspector of its own.
set(A11_FLOW_TOOL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/tool/lsp.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/tool/main.cc
    PARENT_SCOPE)
