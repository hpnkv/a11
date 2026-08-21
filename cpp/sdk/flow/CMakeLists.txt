# The Flow standard library: the actions a composition needs to be a program
# rather than only a description of other people's deployed work. Its own target
# so that a11_actions gains no dependency on the filesystem, on processes or on
# the network, and so a host can leave the whole thing out by not linking it.
set(A11_FLOW_ACTIONS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/ports.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/options.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/policy.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/stop.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/time_actions.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/fs_actions.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/sandbox.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/process_actions.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/system_actions.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/flow_actions.cc
    PARENT_SCOPE)
