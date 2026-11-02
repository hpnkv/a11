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

#ifndef A11_WASM_SHIMS_THREAD_BOOST_PRIMITIVES_H_
#define A11_WASM_SHIMS_THREAD_BOOST_PRIMITIVES_H_

// The WASM module is entered synchronously on one JS agent. ByteReassembler
// retains native packet logic while its native fiber mutex maps to this lock.
namespace thread {
class Mutex {
 public:
  void Lock() {}

  void Unlock() {}
};

class MutexLock {
 public:
  explicit MutexLock(Mutex* mu) : mu_(mu) { mu_->Lock(); }

  ~MutexLock() { mu_->Unlock(); }

  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

 private:
  Mutex* mu_;
};
}  // namespace thread

#endif  // A11_WASM_SHIMS_THREAD_BOOST_PRIMITIVES_H_
