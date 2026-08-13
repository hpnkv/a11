# HTTP as Actions. A thin layer over a11::net -- the schemas, the two handlers,
# and the decoding web-fetch does -- kept as its own target so a11_net does not
# gain a dependency on the actions and nodes layers above it.
set(A11_HTTP_ACTIONS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/http_actions.cc
    PARENT_SCOPE)
