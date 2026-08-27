package a11

import a11.net.WebSocketWireStream
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.jupiter.api.Assumptions.assumeTrue
import java.io.File
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * Live interop: the Kotlin [WebSocketWireStream] (JDK RFC 6455 / HTTP/1.1)
 * against the native C++ `WebSocketWireServer`, spawned via the repo venv. This
 * exercises the byte-chunking `ChannelWireStream` wire format across the
 * Kotlin↔native boundary, which the native HTTP/1.1 support newly makes
 * reachable. Skips when the venv python is unavailable.
 */
class NativeWebSocketInteropTest {
    private fun <T> ok(v: StatusOr<T>): T = (v as Ok<T>).value

    private val venvPython = File("../.venv/bin/python")

    private val serverScript = """
import asyncio, a11
from a11.net.websocket_wire_stream import WebSocketServerOptions, WebSocketWireServer
from a11.service.session import Session
async def main():
    async def on_stream(s):
        async def om(m, st, se):
            if m is None:
                st.half_close(); return
            se.send(m)
        sess = Session(on_stream_message=om)
        await sess.add_stream(s, mode='accept')
        await sess.done.wait()
    o = WebSocketServerOptions(); o.path = '/a11'
    srv = WebSocketWireServer.create(on_stream, o)
    print('PORT=' + str(srv.port), flush=True)
    await asyncio.sleep(30)
asyncio.run(main())
""".trimIndent()

    @Test
    fun kotlinClientEchoesThroughNativeServer(): Unit = runBlocking {
        assumeTrue(venvPython.canExecute(), "repo venv python not available; skipping")

        val process = ProcessBuilder(venvPython.absolutePath, "-c", serverScript).start()
        try {
            val reader = process.inputStream.bufferedReader()
            var port = -1
            withTimeout(15_000) {
                while (true) {
                    val line = reader.readLine() ?: break
                    if (line.startsWith("PORT=")) { port = line.substring(5).trim().toInt(); break }
                }
            }
            assertTrue(port > 0, "native server did not report a port")

            val received = CompletableDeferred<WireMessage?>()
            val session = Session(
                actionRegistry = ActionRegistry(),
                id = "interop",
                onStreamMessage = { message, _, _ ->
                    if (!received.isCompleted) received.complete(message)
                    Status.ok()
                },
            )
            val stream = ok(WebSocketWireStream.connect("ws://127.0.0.1:$port/a11"))
            withTimeout(10_000) {
                assertTrue(session.addStream(stream, StreamMode.START).isOk)
                val msg = WireMessage(
                    nodeFragments = mutableListOf(
                        NodeFragment(id = "n", data = Chunk(data = "kotlin-native".toByteArray()), seq = 0, continued = false),
                    ),
                )
                assertTrue(session.send(msg).isOk)
                val echo = received.await()
                assertTrue(echo != null, "no echo from native server")
                val chunk = ok(echo!!.nodeFragments[0].getChunk())
                assertEquals("kotlin-native", String(chunk.data))
            }
        } finally {
            process.destroyForcibly()
        }
    }

    /** Whether something is listening on a loopback port yet. */
    private fun portAccepts(port: Int): Boolean =
        try {
            java.net.Socket("127.0.0.1", port).close()
            true
        } catch (_: java.io.IOException) {
            false
        }

    // The caller's half of `__list_actions__`. Spelled here rather than taken
    // from the builtin table on purpose: a client needs it to build the call
    // before it has asked anything, and a copy that had drifted from the
    // gateway's is exactly what this test should catch.
    private val listActionsSchema = ActionSchema(
        name = LIST_ACTIONS_NAME,
        inputs = linkedMapOf("request" to ActionPortSchema("request", JSON_MIMETYPE, unary = true)),
        outputs = linkedMapOf(
            "actions" to ActionPortSchema("actions", JSON_MIMETYPE, required = true, unary = true),
        ),
    )

    @Test
    fun kotlinPluginFlowAgainstRealGateway(): Unit = runBlocking {
        // The plugin talks to `a11 gateway`, so this drives the same service
        // the IDE does rather than a test double of it.
        val gateway = File("../.venv/bin/a11")
        assumeTrue(gateway.canExecute(), "repo venv a11 CLI not available; skipping")

        val port = 8_713
        val process = ProcessBuilder(
            gateway.absolutePath, "gateway",
            "--a11-port", port.toString(),
            "--no-audio-capture", "--no-speech-recognition",
            "--conversation-store-root", File.createTempFile("a11-gw", "").let {
                it.delete(); it.mkdirs(); it.absolutePath
            },
        ).redirectErrorStream(true).start()
        try {
            // The gateway does not announce its port, so wait for it to answer
            // on the one it was given.
            withTimeout(20_000) {
                while (!portAccepts(port)) {
                    assertTrue(process.isAlive, "gateway exited before listening")
                    Thread.sleep(200)
                }
            }

            // The plugin's ensureConnected(), and then the thing that replaced
            // its announce handshake: asking the gateway what it serves.
            val stream = ok(WebSocketWireStream.connect("ws://127.0.0.1:$port/a11"))
            val session = Session(actionRegistry = ActionRegistry(), id = "plugin-flow")
            val added = session.addStream(stream, StreamMode.START)
            assertTrue(added.isOk, "addStream failed: $added")

            val ask = Action.create(
                listActionsSchema,
                ActionCreateOptions(session = session, stream = stream),
            ).valueOrThrow()
            ask.call().valueOrThrow()
            ask.getInput("request").valueOrThrow().finalize()
            val response = withTimeout(15_000) {
                ask.getOutput("actions", bindStream = false).valueOrThrow().next(timeoutMs = 10_000)
            }
            // The value, not the StatusOr around it: `next` answers `Ok(null)`
            // at finality, so the read has to be unwrapped before it says
            // whether anything arrived.
            assertNotNull(
                ok(response),
                "__list_actions__ produced no 'actions' output",
            )

            // Kotlin must parse the schema document returned by the native
            // gateway.
            val document = ok(response) as Map<*, *>
            assertEquals(SCHEMA_DOCUMENT_FORMAT, document["format"])
            val entries = ok(schemasInDocument(document))
            assertTrue(entries.isNotEmpty(), "the gateway described no actions")
            for (entry in entries) {
                val schema = ok(schemaFromJson(entry))
                assertTrue(schema.name.isNotEmpty())
            }
            // The gateway serves the flow tools, so at least one is nameable.
            assertTrue(
                entries.any { it["name"] == "flow_run" },
                "expected flow_run among ${entries.map { it["name"] }}",
            )
        } finally {
            process.destroyForcibly()
        }
    }
}
