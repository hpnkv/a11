# WebSocket echo in Kotlin

Run a local native A11 WebSocket server and a Kotlin coroutine client, as in
example 000. The client sends the `hello: world` handshake header and text chunks
to `/ws`. Enter `exit` or close stdin to half-close and drain the session.

Use JDK 21 and the native build prerequisites in [BUILDING.md](../../BUILDING.md).
The Gradle composite builds this checkout's Kotlin library and packages its native
library; it does not require a published artifact.

```sh
kotlin/gradlew -p examples/010-websocket-echo-kotlin run --console=plain
```

To build an executable distribution:

```sh
kotlin/gradlew -p examples/010-websocket-echo-kotlin installDist
examples/010-websocket-echo-kotlin/build/install/a11-websocket-echo-kotlin/bin/a11-websocket-echo-kotlin
```

Kotlin formatting follows the repository's `.editorconfig` and Google's Kotlin
style guide. Run `kotlin/gradlew -p kotlin kotlinFormatCheck` to check the library
and this example together.
