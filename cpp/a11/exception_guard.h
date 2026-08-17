// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Turns what a caller's callable throws into a Status, at the boundary.
 *
 * A11 is compiled `-fno-exceptions` (see `a11_disallow_exceptions` in
 * cpp/CMakeLists.txt) and raises nothing of its own. A callable handed *in* by
 * a caller is the exception: it may come from C++ built with exceptions, or
 * from Python through pybind11, and it may throw. This is where that is dealt
 * with, and there are exactly two shapes.
 *
 * ## Wrap, for a callable A11 stores and calls later
 *
 * A `try` only protects a callable it invokes directly. An exception raised by
 * a callable that A11 invokes from one of its own frames would have to unwind
 * through that frame, and a `-fno-exceptions` frame has no cleanup information:
 * the destructors of its locals do not run and the behaviour is undefined. So a
 * helper of the shape
 *
 * exception_guard::Attempt([&] { on_message(message); }, "on_message");   //
 * WRONG here
 *
 * protects nothing when it sits in one of A11's own translation units -- the
 * lambda belongs to that unit, and the unit has no exceptions.
 *
 * `Wrap` instead returns a callable whose body was compiled *with* exceptions,
 * so at call time the stack reads
 *
 *     [A11, no exceptions] -> [the wrapper, exceptions, try] -> [the callable]
 *
 * and nothing unwinds through A11. Wrap once, where the callable is adopted --
 * where `on_message` is stored, where a codec is registered -- and every later
 * invocation is safe without another thought. This is the shape to use for
 * anything crossing A11's public API.
 *
 * ## Attempt, for a template in a header
 *
 * `Submit<T>` and `Future<T>::OnReady` cannot be wrapped that way: they are
 * templates, instantiated by whoever calls them, so there is no single
 * signature to pre-compile. For those, `Attempt` runs the callable in the frame
 * of the translation unit doing the instantiating, and catches only if that
 * unit has exceptions -- which is exactly the unit whose callable might throw.
 * Inside A11's own build it compiles to a plain call with no landing pad.
 *
 * The caveat, and it is why the public API prefers `Wrap`: if A11 and a caller
 * instantiate the same template with the same types, the linker keeps one of
 * the two bodies, and it may keep A11's. A callable handed to a template entry
 * point should therefore not rely on being caught. Anything type-erased --
 * `OnMessage`, an action handler, a registered codec -- is wrapped and carries
 * the guarantee.
 *
 * ## Adding a Wrap signature
 *
 * `Wrap` is declared here and defined in a11/internal/exception_guard_impl.h,
 * which only the per-library boundary translation units include --
 * `a11/data/boundary.cc` and its siblings, each named in the exception policy
 * block of cpp/CMakeLists.txt. Instantiating a signature there is what makes it
 * usable; forgetting to is a link error naming the exact signature, which is
 * the intended way to find out. The boundary unit belongs to the library that
 * owns the callable's type, so none of this inverts the dependency graph.
 */

#ifndef A11_EXCEPTION_GUARD_H_
#define A11_EXCEPTION_GUARD_H_

#include <exception>
#include <functional>
#include <string_view>
#include <utility>

#include <absl/functional/any_invocable.h>
#include <absl/status/status.h>

namespace a11::exception_guard {

namespace internal {

// / The status `what` raising `error` becomes. Shared so the wording is one
// thing.
absl::Status Raised(const std::exception& error, std::string_view what);
/// The same, for something thrown that is not a std::exception.
absl::Status RaisedUnknown(std::string_view what);

}  // namespace internal

/**
 * @brief Runs @p callable, reporting anything it throws as a Status.
 *
 * For a template in a header, where the instantiating translation unit is both
 * the one that supplies the callable and the one that decides whether
 * exceptions exist. See the file comment: inside A11 this is a plain call, and
 * it is *not* the tool for a callable stored by A11 and invoked later.
 *
 * @param callable Anything invocable with no arguments and no return value.
 * @param what How to name it in the error, e.g. "on_message".
 * @return OK, or what the callable threw.
 */
template <typename Callable>
absl::Status Attempt([[maybe_unused]] Callable&& callable,
                     [[maybe_unused]] std::string_view what) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
  try {
    std::forward<Callable>(callable)();
  } catch (const std::exception& error) {
    return internal::Raised(error, what);
  } catch (...) {
    return internal::RaisedUnknown(what);
  }
  return absl::OkStatus();
#else
  std::forward<Callable>(callable)();
  return absl::OkStatus();
#endif
}

/**
 * @brief Wraps @p callable so that anything it throws becomes a failure.
 *
 * What "a failure" means depends on the return type: an error `absl::Status` or
 * `absl::StatusOr`, a failed `a11::Future`, and for a callable returning `void`
 * a logged error and nothing else -- which is all a caller of a void callback
 * could have done with it anyway.
 *
 * @param callable What the caller handed us. An empty callable is returned as
 * is, so a caller's own emptiness check still sees what it expects. @param what
 * How to name the callable in the error, e.g. "on_message". The message reads
 * `<what> raised: <e.what()>`, or `<what> raised a non-standard exception` for
 * something that is not a std::exception.
 */
template <typename Result, typename... Args>
std::function<Result(Args...)> Wrap(std::function<Result(Args...)> callable,
                                    std::string_view what);

/** @brief `Wrap` for a move-only callable. */
template <typename Result, typename... Args>
absl::AnyInvocable<Result(Args...)> WrapOnce(
    absl::AnyInvocable<Result(Args...)> callable, std::string_view what);

/** @brief `Wrap` for a callable that is consumed by its one invocation. */
template <typename Result, typename... Args>
absl::AnyInvocable<Result(Args...) &&> WrapConsuming(
    absl::AnyInvocable<Result(Args...) &&> callable, std::string_view what);

}  // namespace a11::exception_guard

#endif  // A11_EXCEPTION_GUARD_H_
