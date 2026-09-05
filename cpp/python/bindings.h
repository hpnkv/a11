/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef A11_PYTHON_BINDINGS_H_
#define A11_PYTHON_BINDINGS_H_

#include <pybind11/pybind11.h>

namespace a11::python {

void BindCore(pybind11::module_& module);
void BindData(pybind11::module_& module);
void BindDebug(pybind11::module_& module);
void BindFlow(pybind11::module_& module);
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
