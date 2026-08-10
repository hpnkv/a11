// Copyright 2026 The A11 Authors.

#ifndef A11_PYTHON_BINDINGS_H_
#define A11_PYTHON_BINDINGS_H_

#include <pybind11/pybind11.h>

namespace a11::python {

void BindCore(pybind11::module_& module);
void BindData(pybind11::module_& module);
void BindLogging(pybind11::module_& module);
#ifdef A11_BUILD_REDIS
void BindRedis(pybind11::module_& module);
#endif
#ifdef A11_BUILD_AUDIO
void BindAudio(pybind11::module_& module);
#endif
void BindStores(pybind11::module_& module);
void BindNet(pybind11::module_& module);
void BindHttp(pybind11::module_& module);
void BindWebRtc(pybind11::module_& module);
void BindNodes(pybind11::module_& module);
void BindActions(pybind11::module_& module);
void BindService(pybind11::module_& module);
void BindObs(pybind11::module_& module);

}  // namespace a11::python

#endif  // A11_PYTHON_BINDINGS_H_
