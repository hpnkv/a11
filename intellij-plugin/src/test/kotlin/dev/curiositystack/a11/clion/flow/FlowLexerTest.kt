package dev.curiositystack.a11.clion.flow

import com.intellij.psi.TokenType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test

/**
 * The lexer, which is now a replay of what the language said.
 *
 * Two halves, and they are tested differently on purpose:
 *
 * * **The platform's contract** -- every character in exactly one token, tokens
 *   that advance, a restart from any boundary -- is this plugin's own and holds
 *   whether or not the tool is there. Those cases run always, and with no tool
 *   they exercise the fallback that knows the shape of a flow and none of its
 *   words.
 * * **What a word means** is the language's, and asserting it here would be
 *   asserting it twice: `cpp/tests/flow_highlight_test.cc` is where the judgement
 *   lives. What is checked here is the *translation* -- that a `stage` becomes
 *   `STAGE` and a `{` becomes `LEFT_BRACE` -- which needs the tool, so those cases
 *   skip without one rather than pretending to pass.
 */
class FlowLexerTest {

    private fun tokens(source: String): List<String> {
        val lexer = FlowLexer()
        lexer.start(source, 0, source.length, 0)
        val found = mutableListOf<String>()
        while (true) {
            val type = lexer.tokenType ?: break
            if (type != TokenType.WHITE_SPACE) {
                val text = source.substring(lexer.tokenStart, lexer.tokenEnd)
                found += "${type.toString().removePrefix("A11Flow:")}:$text"
            }
            lexer.advance()
        }
        return found
    }

    /** Every token, whitespace included, so the ranges can be checked. */
    private fun cover(source: String): String {
        val lexer = FlowLexer()
        lexer.start(source, 0, source.length, 0)
        val rebuilt = StringBuilder()
        var previousEnd = 0
        while (lexer.tokenType != null) {
            assertEquals("tokens must not skip input", previousEnd, lexer.tokenStart)
            assertTrue("tokens must advance", lexer.tokenEnd > lexer.tokenStart)
            rebuilt.append(source, lexer.tokenStart, lexer.tokenEnd)
            previousEnd = lexer.tokenEnd
            lexer.advance()
        }
        return rebuilt.toString()
    }

    /** Where a token starts, which is where the platform may relex from. */
    private fun boundaries(source: String): List<Int> {
        val lexer = FlowLexer()
        lexer.start(source, 0, source.length, 0)
        val starts = mutableListOf<Int>()
        while (lexer.tokenType != null) {
            starts += lexer.tokenStart
            lexer.advance()
        }
        // A handful spread through the file, rather than all of them: the point is
        // that a restart works, not that it works 200 times.
        return starts.filterIndexed { index, _ -> index % 7 == 0 }
    }

    private fun withLanguage() {
        assumeTrue(
            "no a11-flow on this machine; build it with `cmake --build" +
                " --preset debug --target a11_flow_tool`",
            FlowEngine.instance().available,
        )
    }

    @Test
    fun `every character is covered by exactly one token`() {
        assertEquals(SOURCE, cover(SOURCE))
    }

    @Test
    fun `a comment and a string are found without the language`() {
        // The fallback's whole job: whatever else is unknown, prose is prose.
        val found = tokens("# a note\n\"text\" 30s 12")
        assertTrue(found.toString(), found.contains("COMMENT:# a note"))
        assertTrue(found.toString(), found.contains("STRING:\"text\""))
        assertTrue(found.toString(), found.contains("DURATION:30s"))
        assertTrue(found.toString(), found.contains("NUMBER:12"))
    }

    @Test
    fun `a triple-quoted string is one token, line breaks and all`() {
        val source = "describe \"\"\"\n  two lines\n  of it\n\"\"\"\n"
        val found = tokens(source)
        assertTrue(
            found.toString(),
            found.any { it.startsWith("STRING:\"\"\"") && it.contains("of it") },
        )
        assertEquals(source, cover(source))
    }

    @Test
    fun `the lexer restarts from any token boundary`() {
        // The platform relexes from the last unchanged token, so starting in the
        // middle of the buffer has to give the same tokens as starting at the top.
        val whole = tokens(SOURCE)
        for (boundary in boundaries(SOURCE)) {
            val lexer = FlowLexer()
            lexer.start(SOURCE, boundary, SOURCE.length, 0)
            val tail = mutableListOf<String>()
            while (lexer.tokenType != null) {
                if (lexer.tokenType != TokenType.WHITE_SPACE) {
                    tail += "${lexer.tokenType.toString().removePrefix("A11Flow:")}:" +
                        SOURCE.substring(lexer.tokenStart, lexer.tokenEnd)
                }
                lexer.advance()
            }
            assertTrue(
                "a restart at $boundary must be a suffix of the whole file",
                whole.takeLast(tail.size) == tail,
            )
        }
    }

    @Test
    fun `a declaration names its flow`() {
        withLanguage()
        assertEquals(
            listOf(
                "DECLARATION_KEYWORD:flow",
                "FLOW_NAME:research",
                "LEFT_BRACE:{",
                "RIGHT_BRACE:}",
            ),
            tokens("flow research {}"),
        )
    }

    @Test
    fun `a keyword may be upper case but not mixed`() {
        withLanguage()
        assertEquals(
            listOf("DECLARATION_KEYWORD:FLOW", "FLOW_NAME:research"),
            tokens("FLOW research"),
        )
        // `Flow` is a name, which is what the compiler makes of it too.
        assertEquals(
            listOf("IDENTIFIER:Flow", "IDENTIFIER:research"),
            tokens("Flow research"),
        )
    }

    @Test
    fun `a port declaration reads as one`() {
        withLanguage()
        assertEquals(
            listOf(
                "DECLARATION_KEYWORD:in",
                "IDENTIFIER:question",
                "COLON::",
                "TYPE:string",
                "DECLARATION_KEYWORD:required",
                "STRING:\"What to find out.\"",
            ),
            tokens("""in question: string required "What to find out.""""),
        )
    }

    @Test
    fun `a call names an action and its modifiers are modifiers`() {
        withLanguage()
        assertEquals(
            listOf(
                "IDENTIFIER:x",
                "ASSIGN:=",
                "STATEMENT_KEYWORD:try",
                "STATEMENT_KEYWORD:call",
                "ACTION_NAME:web-fetch",
                "LEFT_PAREN:(",
                "IDENTIFIER:url",
                "COLON::",
                "IDENTIFIER:q",
                "RIGHT_PAREN:)",
                "MODIFIER_KEYWORD:timeout",
                "DURATION:30s",
                "MODIFIER_KEYWORD:via",
                "NODE_MAP_NAME:scratch",
            ),
            tokens("x = try call web-fetch(url: q) timeout 30s via scratch"),
        )
    }

    @Test
    fun `a stage a function and a status code are told apart`() {
        withLanguage()
        assertEquals(
            listOf("IDENTIFIER:q", "PIPE:|", "STAGE:truncate", "NUMBER:200"),
            tokens("q | truncate 200"),
        )
        assertEquals(
            listOf(
                "STATEMENT_KEYWORD:if",
                "BUILTIN:len",
                "LEFT_PAREN:(",
                "IDENTIFIER:q",
                "RIGHT_PAREN:)",
                "COMPARISON:>",
                "NUMBER:0",
                "LEFT_BRACE:{",
                "STATEMENT_KEYWORD:fail",
                "STATUS_CODE:not_found",
                "STRING:\"no\"",
                "RIGHT_BRACE:}",
            ),
            tokens("""if len(q) > 0 { fail not_found "no" }"""),
        )
    }

    @Test
    fun `a registry tag is a type all the way along`() {
        withLanguage()
        assertEquals(
            listOf(
                "DECLARATION_KEYWORD:out",
                "IDENTIFIER:audio",
                "COLON::",
                "TYPE:a11",
                "DOT:.",
                "TYPE:sdk",
                "DOT:.",
                "TYPE:AudioBuffer",
                "DECLARATION_KEYWORD:stream",
            ),
            tokens("out audio: a11.sdk.AudioBuffer stream"),
        )
    }

    /**
     * A flow with characters outside ASCII in it, coloured where they actually are.
     *
     * This is the regression that made the point. The language counts offsets in
     * bytes; a `CharSequence` counts them in UTF-16 units; for ASCII the two agree,
     * so every example flow in the repository worked and
     * `suggest-fixes.flow` -- which has a `§` in a string -- was coloured
     * from that `§` onwards one column to the left of itself, 638 tokens of 1010
     * wrong. The conversion is the language's now, asked for per request, and what
     * this asserts is that a token's range still contains the token's own text.
     *
     * `§` is two bytes and one unit; the emoji is four bytes and *two* units, so a
     * plugin that had counted code points instead would still fail here.
     */
    @Test
    fun `tokens land on their own text outside ascii`() {
        withLanguage()
        // The wide characters come first, so everything asserted below sits past
        // the drift. The assertions are all *meanings* -- `in` a declaration word,
        // `string` a type, `truncate` a stage -- because those are the one thing the
        // shape fallback cannot produce: it would call all four an identifier. A
        // test that only checked the text would pass on the fallback and prove
        // nothing, which is the first version of this test and why it says so here.
        val source = """
            flow marked {
              describe "a § and an 🙂 walk in"
              in  q: string
              out o: string
              q | truncate 200 -> o
            }
        """.trimIndent()
        // The whole document is still tiled, which is the platform's contract.
        assertEquals(source, cover(source))
        assertEquals(
            listOf(
                "DECLARATION_KEYWORD:flow",
                "FLOW_NAME:marked",
                "LEFT_BRACE:{",
                "DECLARATION_KEYWORD:describe",
                "STRING:\"a § and an 🙂 walk in\"",
                "DECLARATION_KEYWORD:in",
                // A port, not a plain name: this is a whole flow, so the
                // language could resolve `q` and `o` and say they are the
                // interface. In the fragments above there is no flow to be a
                // port of, so they stay identifiers.
                "PORT_NAME:q",
                "COLON::",
                "TYPE:string",
                "DECLARATION_KEYWORD:out",
                "PORT_NAME:o",
                "COLON::",
                "TYPE:string",
                "PORT_NAME:q",
                "PIPE:|",
                "STAGE:truncate",
                "NUMBER:200",
                "ARROW:->",
                "PORT_NAME:o",
                "RIGHT_BRACE:}",
            ),
            tokens(source),
        )
    }

    private companion object {
        val SOURCE = """
            # a flow
            flow research {
              in  question: string required
              out answer:   string stream
              x = try call web-fetch(url: question) timeout 30s via scratch
              x.text | truncate 200 | where it != "" -> answer
              if not (x | count) >= 1 { fail not_found "nothing" }
            }
        """.trimIndent()
    }
}
