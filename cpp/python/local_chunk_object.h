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

#ifndef A11_PYTHON_LOCAL_CHUNK_OBJECT_H_
#define A11_PYTHON_LOCAL_CHUNK_OBJECT_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <pybind11/pybind11.h>

#include "a11/data/types.h"

namespace a11::python {

class PythonChunkObject final : public data::ChunkObject {
 public:
  PythonChunkObject(pybind11::object value, pybind11::object serializer,
                    std::string tag, std::string mimetype);
  ~PythonChunkObject() override;

  [[nodiscard]] std::string_view tag() const override { return tag_; }

  [[nodiscard]] std::string_view mimetype() const override { return mimetype_; }

  [[nodiscard]] absl::StatusOr<data::Bytes> Encode() const override;

  [[nodiscard]] size_t ApproxBytes() const override { return 128; }

  [[nodiscard]] const void* absl_nonnull address() const override {
    return this;
  }

  [[nodiscard]] pybind11::object value() const;
  [[nodiscard]] pybind11::object serializer() const;

 private:
  PyObject* absl_nullable value_ = nullptr;
  PyObject* absl_nullable serializer_ = nullptr;
  std::string tag_;
  std::string mimetype_;
};

data::Chunk MakePythonObjectChunk(pybind11::object value,
                                  pybind11::object serializer, std::string tag,
                                  std::string mimetype);

const PythonChunkObject* absl_nullable PythonObjectIn(const data::Chunk& chunk);

}  // namespace a11::python

#endif  // A11_PYTHON_LOCAL_CHUNK_OBJECT_H_
