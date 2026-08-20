package a11

import a11.net.InProcessWireStream
import a11.sdk.INTERACT_WITH_LLM_SCHEMA
import a11.sdk.LlmHeaders
import a11.sdk.Role
import a11.sdk.makeTextMessageInteraction
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * Drives the full Session/Action/AsyncNode/SDK stack over an in-process wire
 * pair: a client calls `interact_with_llm` on a "backend" session that streams a
 * reply and reverse-dispatches a tool back to the client — the same shape as the
 * plugin calling a real backend and exposing IDE tools.
 */
class EndToEndTest {
    private fun <T> ok(value: StatusOr<T>): T = (value as Ok<T>).value

    private val getActiveFileSchema = ActionSchema(
        name = "get_active_file",
        description = "Return the path of the active editor file.",
        outputs = linkedMapOf("path" to ActionPortSchema("path", "text/plain", required = true)),
    )

    @Test
    fun clientCallsInteractWithLlmAndReceivesStreamedReplyAndToolRoundTrip() = runBlocking {
        withTimeout(15_000) {
            val toolInvoked = AtomicBoolean(false)

            // The fake backend handler: reverse-call the client tool, then stream a reply.
            val backendHandler: ActionHandler = handler@{ action ->
                val tool = action.makeNested("get_active_file").orElse { return@handler it }
                val started = tool.call()
                if (started is Status && !started.isOk) return@handler started
                val toolOut = tool.getOutput("path", bindStream = false).orElse { return@handler it }
                val path = toolOut.next(timeoutMs = 10_000).orElse { return@handler it } as? String
                tool.wait(10_000)

                val text = action.getOutput("text_output").orElse { return@handler it }
                for (token in listOf("active file is ", path ?: "?")) text.put(token)
                text.finalize()

                val newInteractions = action.getOutput("new_interactions").orElse { return@handler it }
                newInteractions.finalize(ok(makeTextMessageInteraction("done", role = Role.ASSISTANT)))
                Status.ok()
            }

            val (clientStream, backendStream) = InProcessWireStream.createPair()

            // Backend: hosts interact_with_llm and a schema-only tool that dispatches back.
            val backendRegistry = ActionRegistry()
            backendRegistry.register("interact_with_llm", INTERACT_WITH_LLM_SCHEMA, backendHandler)
            backendRegistry.register("get_active_file", getActiveFileSchema, handler = null)
            val backendSession = Session(actionRegistry = backendRegistry, id = "session-backend")
            backendSession.addStream(backendStream, StreamMode.ACCEPT)

            // Client: exposes the real IDE tool and drives the LLM call.
            val clientRegistry = ActionRegistry()
            clientRegistry.register("get_active_file", getActiveFileSchema) { toolAction ->
                toolInvoked.set(true)
                val out = toolAction.getOutput("path").orElse { return@register it }
                out.finalize("/src/main.cpp")
                Status.ok()
            }
            val clientSession = Session(actionRegistry = clientRegistry, id = "session-client")
            clientSession.addStream(clientStream, StreamMode.START)

            val call = ok(Action.create(INTERACT_WITH_LLM_SCHEMA, ActionCreateOptions(
                id = "call1", session = clientSession, stream = clientStream, registry = clientRegistry,
            )))
            call.setHeader(LlmHeaders.PROVIDER.header, "claude")
            ok(call.call())

            val interactions = ok(call.getInput("interactions"))
            interactions.finalize(ok(makeTextMessageInteraction("what's open?")))

            val textOut = ok(call.getOutput("text_output", bindStream = false))
            val reply = StringBuilder()
            while (true) {
                val token = textOut.next(timeoutMs = 10_000).orElse { throw AssertionError(it.message) } ?: break
                reply.append(token as String)
            }
            ok(call.wait(10_000))

            assertTrue(toolInvoked.get(), "the client-side tool should have been invoked")
            assertEquals("active file is /src/main.cpp", reply.toString())
        }
    }

    /**
     * A handler that streams with plain puts and then closes still ends the
     * caller's read: nothing here marks a fragment final, so the caller can only
     * finish on the closure marker the drain sends.
     */
    @Test
    fun drainedRemoteOutputEndsTheCallerWithoutAFinalFragment() = runBlocking {
        withTimeout(15_000) {
            val schema = ActionSchema(
                name = "stream_lines",
                outputs = linkedMapOf("lines" to ActionPortSchema("lines", "text/plain", required = true)),
            )
            val (clientStream, serverStream) = InProcessWireStream.createPair()

            val serverRegistry = ActionRegistry()
            serverRegistry.register("stream_lines", schema) { action ->
                val lines = action.getOutput("lines").orElse { return@register it }
                lines.put("first")
                lines.put("second")
                lines.finalize()
            }
            val serverSession = Session(actionRegistry = serverRegistry, id = "session-drain-server")
            serverSession.addStream(serverStream, StreamMode.ACCEPT)

            val clientRegistry = ActionRegistry()
            clientRegistry.register("stream_lines", schema, handler = null)
            val clientSession = Session(actionRegistry = clientRegistry, id = "session-drain-client")
            clientSession.addStream(clientStream, StreamMode.START)

            val call = ok(Action.create(schema, ActionCreateOptions(
                id = "drain1", session = clientSession, stream = clientStream, registry = clientRegistry,
            )))
            ok(call.call())

            val lines = ok(call.getOutput("lines", bindStream = false))
            val received = mutableListOf<String>()
            while (true) {
                val value = lines.next(timeoutMs = 10_000).orElse { throw AssertionError(it.message) } ?: break
                received.add(value as String)
            }
            ok(call.wait(10_000))

            assertEquals(listOf("first", "second"), received)
            assertEquals(false, ok(lines.isWritable()))
        }
    }
}
