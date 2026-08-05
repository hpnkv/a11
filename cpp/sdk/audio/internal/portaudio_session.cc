// Copyright 2026 The A11 Authors.

#include "sdk/audio/internal/portaudio_session.h"

#include <memory>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <portaudio.h>

#include "thread/boost_primitives.h"

namespace a11::sdk::audio::internal {
namespace {

thread::Mutex& SessionMutex() {
  static auto* const mu = new thread::Mutex();
  return *mu;
}

std::weak_ptr<PortAudioSession>& SessionCache() {
  static auto* const cache = new std::weak_ptr<PortAudioSession>();
  return *cache;
}

}  // namespace

absl::Status PaErrorToStatus(std::string_view context, PaError error) {
  const char* text = Pa_GetErrorText(error);
  return absl::InternalError(
      absl::StrCat(context, ": ", text == nullptr ? "unknown error" : text));
}

absl::StatusOr<std::shared_ptr<PortAudioSession>> PortAudioSession::Acquire() {
  // Serialize initialization with the final holder's teardown. PortAudio's
  // global Pa_Initialize/Pa_Terminate state is not safe to transition in both
  // directions concurrently.
  thread::MutexLock lock(&SessionMutex());
  if (std::shared_ptr<PortAudioSession> existing = SessionCache().lock()) {
    return existing;
  }
  const PaError error = Pa_Initialize();
  if (error != paNoError) {
    return PaErrorToStatus("Pa_Initialize", error);
  }
  std::shared_ptr<PortAudioSession> session(new PortAudioSession());
  SessionCache() = session;
  return session;
}

PortAudioSession::~PortAudioSession() {
  thread::MutexLock lock(&SessionMutex());
  Pa_Terminate();
}

}  // namespace a11::sdk::audio::internal
