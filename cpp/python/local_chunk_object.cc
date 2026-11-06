// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "python/local_chunk_object.h"

#include <exception>
#include <utility>

#include <Python.h>
#include <absl/status/status.h>
#include <pybind11/stl.h>

#include "python/interop.h"

namespace py = pybind11;

namespace a11::python {

PythonChunkObject::PythonChunkObject(py::object value, py::object serializer,
                                     std::string tag, std::string mimetype)
    : value_(value.inc_ref().ptr()),
      serializer_(serializer.inc_ref().ptr()),
      tag_(std::move(tag)),
      mimetype_(std::move(mimetype)) {}

PythonChunkObject::~PythonChunkObject() {
  DeferredPythonRefs::Retire(value_);
  DeferredPythonRefs::Retire(serializer_);
}

absl::StatusOr<data::Bytes> PythonChunkObject::Encode() const {
  const py::gil_scoped_acquire acquire;
  try {
    py::object serializer = py::reinterpret_borrow<py::object>(serializer_);
    py::object result = serializer(py::reinterpret_borrow<py::object>(value_));
    if (py::isinstance<py::str>(result)) {
      Py_ssize_t size = 0;
      const char* bytes = PyUnicode_AsUTF8AndSize(result.ptr(), &size);
      if (bytes == nullptr) {
        throw py::error_already_set();
      }
      return std::string(bytes, static_cast<size_t>(size));
    }
    if (py::isinstance<py::bytes>(result) ||
        py::isinstance<py::bytearray>(result) ||
        py::isinstance<py::memoryview>(result)) {
      return py::bytes(result).cast<std::string>();
    }
    return absl::InvalidArgumentError(
        "A local object serializer must return text or bytes.");
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}

py::object PythonChunkObject::value() const {
  const py::gil_scoped_acquire acquire;
  return py::reinterpret_borrow<py::object>(value_);
}

py::object PythonChunkObject::serializer() const {
  const py::gil_scoped_acquire acquire;
  return py::reinterpret_borrow<py::object>(serializer_);
}

data::Chunk MakePythonObjectChunk(py::object value, py::object serializer,
                                  std::string tag, std::string mimetype) {
  data::Chunk chunk;
  chunk.metadata = data::ChunkMetadata{.mimetype = mimetype};
  chunk.object = std::make_shared<const PythonChunkObject>(
      std::move(value), std::move(serializer), std::move(tag),
      std::move(mimetype));
  return chunk;
}

const PythonChunkObject* PythonObjectIn(const data::Chunk& chunk) {
  return dynamic_cast<const PythonChunkObject*>(chunk.object.get());
}

}  // namespace a11::python
