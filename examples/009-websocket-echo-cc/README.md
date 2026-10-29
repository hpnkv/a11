# WebSocket echo in C++

Run a local A11 WebSocket server and an interactive client, as in example 000.
The client sends the `hello: world` handshake header and text chunks to `/ws`.
Enter `exit` or close stdin to half-close and drain the session.

Build against an installed A11 CMake package:

```sh
cmake -S examples/009-websocket-echo-cc -B build/echo-cc \
  -DCMAKE_PREFIX_PATH="/path/to/a11/install;$A11_DEPS_PREFIX"
cmake --build build/echo-cc
./build/echo-cc/websocket-echo
```

From a native build of this checkout, create that installation with
`cmake --install cmake-build-debug --prefix "$PWD/install"`.
