// Copyright 2026 The A11 Authors.

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

#include <absl/debugging/failure_signal_handler.h>
#include <absl/debugging/symbolize.h>
#include <absl/status/status.h>
#include <pybind11/pybind11.h>
#include <pybind11_abseil/status_casters.h>

#include "a11/data/types.h"
#include "a11/status.h"
#include "python/bindings.h"
#include "python/casters.h"

namespace py = pybind11;

namespace {

// Installs Abseil's failure signal handler so fatal signals (SIGSEGV, SIGABRT,
// ...) print a symbolized stack trace instead of a bare crash. Opt out with
// A11_DISABLE_FAILURE_SIGNAL_HANDLER for environments (debuggers, some test
// harnesses) that want to own signal handling.
void InstallFailureSignalHandler() {
  if (std::getenv("A11_DISABLE_FAILURE_SIGNAL_HANDLER") != nullptr) {
    return;
  }
  std::string program;
  try {
    program =
        py::module_::import("sys").attr("executable").cast<std::string>();
  } catch (...) {
    program.clear();
  }
  absl::InitializeSymbolizer(program.c_str());
  absl::InstallFailureSignalHandler(absl::FailureSignalHandlerOptions{});
}

}  // namespace

PYBIND11_MODULE(_native, module) {
  InstallFailureSignalHandler();
  py::google::ImportStatusModule();

  module.doc() = "Native C++ backend for A11";
  module.attr("__version__") = "0.1.0";
  module.attr("EMPTY_WIRE_MESSAGE_SIZE") = a11::data::EmptyWireMessageSize();

  module.def("status_code_from_http", [](int code) {
    return static_cast<int>(a11::StatusCodeFromHttp(code));
  });
  module.def("status_code_to_http", [](int code) {
    return a11::StatusCodeToHttp(static_cast<absl::StatusCode>(code));
  });
  module.def("status_code_from_websocket", [](int code) {
    if (code < 0 ||
        code > static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
      return static_cast<int>(absl::StatusCode::kUnknown);
    }
    return static_cast<int>(
        a11::StatusCodeFromWebSocket(static_cast<std::uint16_t>(code)));
  });
  module.def("status_code_to_websocket", [](int code) {
    return a11::StatusCodeToWebSocket(static_cast<absl::StatusCode>(code));
  });

  // Value types must be registered before any API uses them in a signature.
  a11::python::BindCore(module);
  a11::python::BindData(module);

  // Net is registered first because store writers and later runtime classes
  // accept WireStream base pointers in their signatures.
  a11::python::BindNet(module);
  a11::python::BindHttp(module);
  a11::python::BindWebRtc(module);
  a11::python::BindStores(module);
  a11::python::BindNodes(module);
  // Session is registered before Action because Action accepts a Session in
  // its constructor. ActionRegistry is registered by BindActions below; all
  // conversions happen after module initialization has completed.
  a11::python::BindService(module);
  a11::python::BindActions(module);
  a11::python::BindObs(module);
}
