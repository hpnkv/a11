// Copyright 2026 The A11 Authors.

#ifndef A11_PYTHON_CASTERS_H_
#define A11_PYTHON_CASTERS_H_

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11_abseil/status_casters.h>

// A11 data records are registered as first-class pybind11 classes by
// data_bindings.cc. Keeping this shared include means every binding translation
// unit sees the standard STL and Abseil casters without a serialization-based
// duplicate type caster.

#endif  // A11_PYTHON_CASTERS_H_
