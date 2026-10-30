/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package dev.curiositystack.a11.clion.flow

import org.junit.Assert.assertEquals
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.security.MessageDigest

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

    @Test
    fun `packaged service matches the current native build`() {
        val resource = System.getProperty("a11.flow.bundled.resource")
        assertNotNull("the build must identify its packaged language service", resource)
        val bytes = javaClass.classLoader.getResourceAsStream(resource!!)?.use { it.readBytes() }
        assertNotNull("the language service must be present in plugin resources", bytes)
        val expected = File(System.getProperty("a11.flow.built.tool")).readBytes()
        val hash = MessageDigest.getInstance("SHA-256")
        assertArrayEquals(hash.digest(expected), hash.digest(bytes!!))
    }

    @Test
    fun `bundled service analyses and highlights current Flow syntax`() {
        val source = """
            struct Report exported { title: string }
            declare flow fetch exported { out text: string stream }
            flow render exported {
              out report: Report
              text = take ((run fetch()).text | consume)
              Report{title: text} -> report
            }
        """.trimIndent()
        val checked = engine().check(source)
        assertNotNull(checked)
        val errors = (checked!!["diagnostics"] as List<*>).filter {
            (it as Map<*, *>)["severity"] == "error"
        }
        assertTrue(errors.toString(), errors.isEmpty())
        val payload = engine().tokens(source)
        assertNotNull(payload)
        val tokens = payload!!["tokens"] as List<*>
        for (word in listOf("declare", "exported", "consume")) {
            val start = source.indexOf(word)
            val token = tokens.map { it as Map<*, *> }.first {
                (it["start"] as Number).toInt() == start
            }
            assertTrue(token.toString(), token["kind"] != "identifier")
        }
    }

    @Test
    fun `unresolved qualified calls report errors on their target`() {
        val source = "flow main { run nested::test() }"
        val checked = engine().check(source)!!
        val diagnostics = checked["diagnostics"] as List<*>
        val unknown = diagnostics.map { it as Map<*, *> }.single {
            it["code"] == "flow.name.unknown-call"
        }
        assertEquals("error", unknown["severity"])
        assertTrue(unknown["message"].toString().contains("nested::test"))
        val start = (unknown["range"] as Map<*, *>)["start"] as Map<*, *>
        assertEquals(source.indexOf("nested::test"), (start["offset"] as Number).toInt())
    }

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
            // A stage the language does not have. (`flatten` stood here until
            // the language gained it, on 2026-08-20.)
            "flow t {\n  in q: string\n  out a: string\n  q | shuffle -> a\n}\n",
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
    fun `scope barriers use native diagnostics and completion`() {
        val invalid = engine().check("flow main { wait all timeout 1s }")!!
        assertTrue(invalid["diagnostics"].toString().contains("flow.form.scope-barrier"))
        val source = "flow main { wait "
        val proposals = engine().complete(source, source.length)!!["proposals"] as List<*>
        val barrier = proposals.map { it as Map<*, *> }.first { it["name"] == "all" }
        assertTrue(barrier.toString().contains(
            "Wait for all preceding work in this scope before activating following statements."
        ))
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
