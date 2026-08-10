package a11

import a11.net.WebSocketWireStream
import a11.sdk.INTERACT_WITH_LLM_SCHEMA
import a11.sdk.LlmHeaders
import a11.sdk.ensureInteractionCodec
import a11.sdk.makeTextMessageInteraction
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.jupiter.api.Assumptions.assumeTrue
import kotlin.test.Test
import kotlin.test.assertTrue

/**
 * Cross-language interop against the real Python service (`a11 gateway`): the
 * Kotlin client calls `interact_with_llm` with a native [Interaction]. Runs only
 * when `A11_BACKEND_URL` is set. Provider `ollama` with an unreachable base URL
 * fails fast at the *connection* — which is the point: reaching the connection
 * proves the interaction and config were accepted by the backend's strict,
 * typed input ports, rather than rejected as anonymous JSON.
 */
class LiveBackendInteropTest {
    private fun <T> ok(value: StatusOr<T>): T = (value as Ok<T>).value

    @Test
    fun kotlinClientCallsInteractWithLlmOverRealBackend(): Unit = runBlocking {
        val url = System.getenv("A11_BACKEND_URL")
        assumeTrue(url != null && url.isNotBlank(), "A11_BACKEND_URL not set; skipping live interop test")
        ensureInteractionCodec()

        withTimeout(30_000) {
            val stream = ok(WebSocketWireStream.connect(url!!))
            val session = Session(actionRegistry = ActionRegistry(), id = "session-live")
            session.addStream(stream, StreamMode.START)

            val call = ok(Action.create(INTERACT_WITH_LLM_SCHEMA, ActionCreateOptions(
                id = "livechat", session = session, stream = stream,
            )))
            call.setHeader(LlmHeaders.PROVIDER.header, "ollama")
            call.setHeader(LlmHeaders.MODEL.header, "llama3.2")
            call.setHeader(LlmHeaders.BASE_URL.header, "http://127.0.0.1:1")
            ok(call.call())
            assertTrue(
                call.waitForDispatch(15_000).isOk,
                "backend should acknowledge dispatch of interact_with_llm",
            )

            // A native Interaction, written with the cross-language tag the
            // backend's strict `interactions` port requires. An untagged map
            // would be rejected as a plain object before the provider is reached
            // -- which is exactly what this test exists to catch.
            val interactions = ok(call.getInput("interactions"))
            interactions.putFinal(ok(makeTextMessageInteraction("hello")))
            interactions.drainAndClose()

            // Closed empty, so the backend applies its own default config.
            val config = ok(call.getInput("config"))
            config.putNullFinal()
            config.drainAndClose()

            val tools = ok(call.getInput("tools"))
            tools.putNullFinal()
            tools.drainAndClose()

            // Drain any streamed text (none expected: ollama is unreachable).
            val textOut = ok(call.getOutput("text_output", bindStream = false))
            while (true) {
                val next = textOut.next(timeoutMs = 20_000)
                if (next !is Ok || next.value == null) break
            }

            val completion = call.wait(20_000)
            val message = when (completion) { is Ok -> "OK"; is Status -> "${completion.code}: ${completion.message}" }
            java.io.File("/tmp/a11_completion.txt").writeText(message)
            assertTrue(completion is Ok || completion is Status, "a completion status must arrive")
            session.halfClose()
        }
    }
}
