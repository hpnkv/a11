package dev.curiositystack.a11.clion.flow

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test

/**
 * The one thing this plugin still has to get right about the language: talking to
 * it.
 *
 * Not what a stage is, not which names are in scope, not what counts as a problem
 * -- all of that is `cpp/a11/flow/` and is tested there. What is this plugin's
 * business is the protocol: that a request goes out, an answer comes back, and the
 * envelopes are the ones every other frontend reads.
 *
 * Skipped without a built tool, because a test that quietly passed on a machine
 * with no binary would be a test that says nothing.
 */
class FlowEngineTest {

    private fun engine(): FlowEngine {
        val engine = FlowEngine.instance()
        assumeTrue(
            "no a11-flow on this machine; build it with `cmake --build" +
                " --preset debug --target a11_flow_tool`",
            engine.available,
        )
        return engine
    }

    @Test
    fun `a flow with a mistake in it comes back as diagnostics`() {
        val payload = engine().check(
            "flow t {\n  in q: string\n  out a: string\n  q | flatten -> a\n}\n",
        )
        assertNotNull(payload)
        assertEquals("flow.diagnostics/v1", payload!!["format"])
        val diagnostics = payload["diagnostics"] as List<*>
        assertTrue(diagnostics.toString(), diagnostics.isNotEmpty())
        val first = diagnostics.first() as Map<*, *>
        // The precise code, from the native parser, and the range in offsets --
        // which is what the annotator draws with.
        assertEquals("flow.form.unknown-stage", first["code"])
        assertEquals("error", first["severity"])
        val start = (first["range"] as Map<*, *>)["start"] as Map<*, *>
        assertTrue((start["offset"] as Number).toInt() > 0)
    }

    @Test
    fun `every token comes back with what it means and what it is`() {
        val payload = engine().tokens("flow t { }")
        assertNotNull(payload)
        assertEquals("flow.tokens/v1", payload!!["format"])
        val tokens = payload["tokens"] as List<*>
        val first = tokens.first() as Map<*, *>
        assertEquals("declaration-keyword", first["kind"])
        // The lexer's own name for it, which is what the replay lexer needs for
        // punctuation the platform matches on.
        assertEquals("word", first["lexical"])
    }

    @Test
    fun `formatting gives back the text and whether it changed`() {
        val payload = engine().format("flow   t {  }\n")
        assertNotNull(payload)
        assertEquals("flow.format/v1", payload!!["format"])
        assertEquals(true, payload["changed"])
        // A `{` opens a block, so the formatter puts the `}` on its own line.
        assertEquals("flow t {\n}\n", payload["formatted"])
    }

    @Test
    fun `completion is offered for one offset`() {
        val source = "flow t {\n  in q: string\n  q | "
        val payload = engine().complete(source, source.length)
        assertNotNull(payload)
        assertEquals("flow.completions/v1", payload!!["format"])
        val names = (payload["proposals"] as List<*>).map {
            (it as Map<*, *>)["name"]
        }
        assertTrue(names.toString(), names.contains("truncate"))
        // Only stages after a `|`: no names, no functions.
        assertTrue(names.toString(), !names.contains("len"))
    }

    @Test
    fun `the same text is answered from the last answer`() {
        val engine = engine()
        val source = "flow t {\n  in q: string\n  out a: string\n  q -> a\n}\n"
        val first = engine.check(source)
        val second = engine.check(source)
        // Same object, not merely an equal one: a document that has not changed
        // must not cost a round trip per keystroke elsewhere in the IDE.
        assertTrue(first === second)
    }
}
