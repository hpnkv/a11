// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Python.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/typing.h>

#include "a11/concurrency/future.h"
#include "python/bindings.h"
#include "python/interop.h"
#include "redis/client.h"
#include "redis/reply.h"

namespace a11::python {
namespace {

std::string RedisBytes(const py::handle& value, const char* name) {
  try {
    if (py::isinstance<py::str>(value)) {
      return value.cast<std::string>();
    }
    if (py::isinstance<py::bytes>(value) ||
        PyByteArray_Check(value.ptr()) != 0 ||
        PyMemoryView_Check(value.ptr()) != 0) {
      const auto borrowed = py::reinterpret_borrow<py::object>(value);
      return py::bytes(borrowed).cast<std::string>();
    }
    ThrowStatus(absl::InvalidArgumentError(std::string(name) +
                                           " must be str or bytes-like"));
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  } catch (...) {
    ThrowStatus(absl::UnknownError(std::string("Converting ") + name +
                                   " raised an exception"));
  }
}

std::vector<std::string> RedisParts(const py::handle& values,
                                    const char* name) {
  if (py::isinstance<py::str>(values) || py::isinstance<py::bytes>(values) ||
      PyByteArray_Check(values.ptr()) != 0 ||
      PyMemoryView_Check(values.ptr()) != 0) {
    ThrowStatus(absl::InvalidArgumentError(
        std::string(name) + " must be a sequence, not one byte string"));
  }
  try {
    std::vector<std::string> result;
    for (const py::handle value : values) {
      result.push_back(RedisBytes(value, name));
    }
    return result;
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  } catch (...) {
    ThrowStatus(absl::UnknownError(std::string("Converting ") + name +
                                   " raised an exception"));
  }
}

absl::Time RedisDeadline(const py::handle& value) {
  return ValueOrThrow(TimeFromPython(value));
}

void CheckStatus(const absl::Status& status) {
  if (!status.ok()) {
    ThrowStatus(status);
  }
}

void ValidateClientOptions(const redis::ClientOptions& options) {
  CheckStatus(options.Validate());
}

py::bytes ReplyBytes(const redis::Reply& reply) {
  const std::string_view value = ValueOrThrow(reply.AsStringView());
  return py::bytes(value.data(), value.size());
}

}  // namespace

void BindRedis(py::module_& module) {
  py::enum_<redis::ReplyType>(module, "RedisReplyType",
                              "The RESP value kind held by RedisReply.")
      .value("NULL", redis::ReplyType::kNull)
      .value("STRING", redis::ReplyType::kString)
      .value("INTEGER", redis::ReplyType::kInteger)
      .value("DOUBLE", redis::ReplyType::kDouble)
      .value("BOOLEAN", redis::ReplyType::kBoolean)
      .value("ARRAY", redis::ReplyType::kArray)
      .value("MAP", redis::ReplyType::kMap)
      .value("SET", redis::ReplyType::kSet);

  py::class_<redis::Reply>(
      module, "RedisReply",
      "An owned, binary-safe RESP value returned by RedisClient.")
      .def_property_readonly("type", &redis::Reply::type,
                             "The RESP kind of this value.")
      .def_property_readonly("is_null", &redis::Reply::is_null,
                             "Whether this is a null RESP value.")
      .def("as_bytes", &ReplyBytes,
           "Return string payload bytes, raising for another RESP kind.")
      .def(
          "as_integer",
          [](const redis::Reply& self) {
            return ValueOrThrow(self.AsInteger());
          },
          "Return the integer payload, raising for another RESP kind.")
      .def(
          "as_float",
          [](const redis::Reply& self) {
            return ValueOrThrow(self.AsDouble());
          },
          "Return the double payload, raising for another RESP kind.")
      .def(
          "as_boolean",
          [](const redis::Reply& self) {
            return ValueOrThrow(self.AsBoolean());
          },
          "Return the boolean payload, raising for another RESP kind.")
      .def(
          "as_elements",
          [](const redis::Reply& self) {
            const std::vector<redis::Reply>* elements =
                ValueOrThrow(self.AsElements());
            return *elements;
          },
          "Return aggregate children; maps alternate key and value entries.")
      .def("debug_string", &redis::Reply::DebugString,
           "Return a diagnostic representation of the RESP value.")
      .def("__bytes__", &ReplyBytes, "Return the bytes held by a string reply.")
      .def("__repr__", &redis::Reply::DebugString,
           "Return a diagnostic representation of the RESP value.");

  py::class_<redis::ClientOptions>(module, "RedisClientOptions",
                                   "Connection and timeout policy for Redis.")
      .def(py::init([](std::string host, int port, std::string username,
                       std::string password, int database,
                       std::string client_name,
                       const py::object& connect_timeout,
                       const py::object& command_timeout) {
             redis::ClientOptions options{
                 .host = std::move(host),
                 .port = port,
                 .username = std::move(username),
                 .password = std::move(password),
                 .database = database,
                 .client_name = std::move(client_name),
                 .connect_timeout = absl::Seconds(10),
                 .command_timeout = absl::Seconds(10),
             };
             if (!connect_timeout.is_none()) {
               options.connect_timeout =
                   ValueOrThrow(DurationFromPython(connect_timeout, false));
             }
             if (!command_timeout.is_none()) {
               options.command_timeout =
                   ValueOrThrow(DurationFromPython(command_timeout, false));
             }
             ValidateClientOptions(options);
             return options;
           }),
           "Construct validated Redis connection options.",
           py::arg("host") = "127.0.0.1", py::arg("port") = 6379,
           py::arg("username") = "", py::arg("password") = "",
           py::arg("database") = 0, py::arg("client_name") = "a11",
           py::arg("connect_timeout") = py::none(),
           py::arg("command_timeout") = py::none())
      .def_readwrite("host", &redis::ClientOptions::host,
                     "Redis host name or IP address.")
      .def_readwrite("port", &redis::ClientOptions::port, "Redis TCP port.")
      .def_readwrite("username", &redis::ClientOptions::username,
                     "ACL username, if authentication is enabled.")
      .def_readwrite("password", &redis::ClientOptions::password,
                     "ACL password, if authentication is enabled.")
      .def_readwrite("database", &redis::ClientOptions::database,
                     "Logical Redis database selected after connection.")
      .def_readwrite("client_name", &redis::ClientOptions::client_name,
                     "Name reported through CLIENT SETNAME.")
      .def_property(
          "connect_timeout",
          [](const redis::ClientOptions& self) -> NativeDuration {
            return NativeDuration(self.connect_timeout);
          },
          [](redis::ClientOptions& self, const py::handle& value) {
            self.connect_timeout =
                ValueOrThrow(DurationFromPython(value, false));
          },
          "Maximum time allowed for establishing a connection.")
      .def_property(
          "command_timeout",
          [](const redis::ClientOptions& self) -> NativeDuration {
            return NativeDuration(self.command_timeout);
          },
          [](redis::ClientOptions& self, const py::handle& value) {
            self.command_timeout =
                ValueOrThrow(DurationFromPython(value, false));
          },
          "Default maximum time allowed for one command.")
      .def("validate", &ValidateClientOptions,
           "Raise if these connection options are invalid.")
      .def_static(
          "from_url",
          [](const std::string& url) {
            return ValueOrThrow(redis::ClientOptions::FromUrl(url));
          },
          "Parse a `redis://[user:password@]host[:port][/database]` URL.",
          py::arg("url"))
      .def_static(
          "from_environment",
          [] { return ValueOrThrow(redis::ClientOptions::FromEnvironment()); },
          "Read A11_REDIS_URL or the individual A11_REDIS_* variables.")
      .def(
          "__eq__",
          [](const redis::ClientOptions& self,
             const redis::ClientOptions& other) { return self == other; },
          py::is_operator());

  py::classh<redis::Subscription>(
      module, "RedisSubscription",
      "A non-buffering broadcast subscription for invalidation events.")
      .def_property_readonly("channel", &redis::Subscription::channel,
                             "The subscribed Redis channel.")
      .def_property_readonly("generation", &redis::Subscription::generation,
                             "The current broadcast generation.")
      .def(
          "wait",
          [](const std::shared_ptr<redis::Subscription>& self,
             std::uint64_t after,
             const py::typing::Optional<NativeTime>& deadline) {
            const absl::Time until = RedisDeadline(deadline);
            return FutureToPython(
                WithoutGil([&] { return self->Wait(after, until); }));
          },
          "Await a message newer than generation `after`.", py::arg("after"),
          py::arg("deadline") = py::none());

  py::classh<redis::Client>(
      module, "RedisClient",
      "An asynchronous, binary-safe hiredis/libuv client using A11 futures.")
      .def(py::init([](redis::ClientOptions options) {
             return ValueOrThrow(redis::Client::Create(std::move(options)));
           }),
           "Create a client and begin connecting without blocking.",
           py::arg("options") = redis::ClientOptions{})
      .def_static(
          "create",
          [](redis::ClientOptions options) {
            return ValueOrThrow(redis::Client::Create(std::move(options)));
          },
          "Create a client and begin connecting without blocking.",
          py::arg("options") = redis::ClientOptions{})
      .def_property_readonly(
          "options", [](const redis::Client& self) { return self.options(); },
          "A copy of the client's connection options.")
      .def(
          "ready",
          [](const std::shared_ptr<redis::Client>& self) {
            return FutureToPython(WithoutGil([&] { return self->Ready(); }));
          },
          "Await initialization of command and Pub/Sub connections.")
      .def(
          "command",
          [](const std::shared_ptr<redis::Client>& self,
             const py::handle& parts,
             const py::typing::Optional<NativeTime>& deadline) {
            std::vector<std::string> converted =
                RedisParts(parts, "Redis command part");
            const absl::Time until = RedisDeadline(deadline);
            return FutureToPython(WithoutGil(
                [&] { return self->Command(std::move(converted), until); }));
          },
          "Execute a binary-safe command supplied as name plus arguments.",
          py::arg("parts"), py::arg("deadline") = py::none())
      .def(
          "eval",
          [](const std::shared_ptr<redis::Client>& self,
             const py::handle& script, const py::handle& keys,
             const py::handle& arguments,
             const py::typing::Optional<NativeTime>& deadline) {
            std::string source = RedisBytes(script, "Redis script");
            std::vector<std::string> key_names =
                RedisParts(keys, "Redis script key");
            std::vector<std::string> values =
                RedisParts(arguments, "Redis script argument");
            const absl::Time until = RedisDeadline(deadline);
            return FutureToPython(WithoutGil([&] {
              return self->Eval(std::move(source), std::move(key_names),
                                std::move(values), until);
            }));
          },
          "Execute Lua while explicitly declaring every cluster-sensitive key.",
          py::arg("script"), py::arg("keys"),
          py::arg("arguments") = py::tuple(), py::arg("deadline") = py::none())
      .def(
          "subscribe",
          [](const std::shared_ptr<redis::Client>& self,
             const py::handle& channel,
             const py::typing::Optional<NativeTime>& deadline) {
            std::string name = RedisBytes(channel, "Redis channel");
            const absl::Time until = RedisDeadline(deadline);
            return FutureToPython(WithoutGil(
                [&] { return self->Subscribe(std::move(name), until); }));
          },
          "Subscribe and await Redis's acknowledgement.", py::arg("channel"),
          py::arg("deadline") = py::none())
      .def(
          "close", [](redis::Client& self) { CheckStatus(self.Close()); },
          "Begin an idempotent asynchronous disconnect.");

  module.def(
      "default_redis_client",
      [] { return ValueOrThrow(redis::DefaultClient()); },
      "Return the process-global client configured from A11_REDIS_*.");
  module.def(
      "set_default_redis_client",
      [](std::shared_ptr<redis::Client> client) {
        CheckStatus(redis::SetDefaultClient(std::move(client)));
      },
      "Replace the process-global Redis client.", py::arg("client"));
  module.def("reset_default_redis_client", &redis::ResetDefaultClient,
             "Clear the global Redis client so its environment is reread.");
}

}  // namespace a11::python
