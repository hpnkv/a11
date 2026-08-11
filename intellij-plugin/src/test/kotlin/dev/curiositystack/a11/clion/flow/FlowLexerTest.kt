package dev.curiositystack.a11.clion.flow

import com.intellij.psi.TokenType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The lexer against the rules in `a11/flow/lexer.py`.
 *
 * These are the cases where a highlighter is easy to get subtly wrong: a dash is
 * part of a name but `->` is not, a word is a keyword only in one case, and what
 * a word means depends on the one token before it.
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

    @Test
    fun `every character is covered by exactly one token`() {
        val source = """
            # a flow
            flow research {
              in  question: string required
              out answer:   string stream
              x = try call web-fetch(url: question) timeout 30s via scratch
              x.text | truncate 200 | where it != "" -> answer
              if not (x | count) >= 1 { fail not_found "nothing" }
            }
        """.trimIndent()
        assertEquals(source, cover(source))
    }

    @Test
    fun `a declaration names its flow`() {
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
        assertEquals(
            listOf("DECLARATION_KEYWORD:FLOW", "FLOW_NAME:research"),
            tokens("FLOW research"),
        )
        // `Flow` is a name, which is what the compiler makes of it too.
        assertEquals(listOf("IDENTIFIER:Flow", "IDENTIFIER:research"), tokens("Flow research"))
    }

    @Test
    fun `a port declaration reads as one`() {
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
        assertEquals(
            listOf(
                "DECLARATION_KEYWORD:out",
                "IDENTIFIER:sources",
                "COLON::",
                "TYPE:string",
                "DECLARATION_KEYWORD:stream",
            ),
            tokens("out sources: string stream"),
        )
    }

    @Test
    fun `a port type may be generic, or a tag, and stops at the line`() {
        // A registry tag is dotted, and the dots are part of the type rather
        // than the accessor they would be anywhere else.
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
        assertEquals(
            listOf(
                "DECLARATION_KEYWORD:in",
                "IDENTIFIER:frames",
                "COLON::",
                "TYPE:list",
                "LEFT_BRACKET:[",
                "TYPE:a11",
                "DOT:.",
                "TYPE:NodeFragment",
                "RIGHT_BRACKET:]",
                "DECLARATION_KEYWORD:required",
            ),
            tokens("in frames: list[a11.NodeFragment] required"),
        )
        // A declaration is one line: the statement under it is a statement.
        assertEquals(
            listOf(
                "DECLARATION_KEYWORD:in",
                "IDENTIFIER:q",
                "COLON::",
                "TYPE:string",
                "IDENTIFIER:page",
                "DOT:.",
                "MEMBER:text",
            ),
            tokens("in q: string\npage.text"),
        )
        // `in` is only a port declaration where a name and a `:` follow it.
        assertEquals(
            listOf(
                "STATEMENT_KEYWORD:for",
                "IDENTIFIER:hit",
                "DECLARATION_KEYWORD:in",
                "IDENTIFIER:search",
                "DOT:.",
                "MEMBER:hits",
            ),
            tokens("for hit in search.hits"),
        )
    }

    @Test
    fun `packb is a stage like any other`() {
        assertEquals(
            listOf("PIPE:|", "STAGE:packb"),
            tokens("| packb"),
        )
    }

    @Test
    fun `a call names an action, and its modifiers are modifiers`() {
        assertEquals(
            listOf(
                "IDENTIFIER:page",
                "ASSIGN:=",
                "STATEMENT_KEYWORD:try",
                "STATEMENT_KEYWORD:call",
                "ACTION_NAME:web-fetch",
                "LEFT_PAREN:(",
                "IDENTIFIER:url",
                "COLON::",
                "IDENTIFIER:hit",
                "DOT:.",
                "MEMBER:url",
                "RIGHT_PAREN:)",
                "MODIFIER_KEYWORD:timeout",
                "DURATION:20s",
                "MODIFIER_KEYWORD:via",
                "NODE_MAP_NAME:fetched",
            ),
            tokens("page = try call web-fetch(url: hit.url) timeout 20s via fetched"),
        )
    }

    @Test
    fun `a run names an action just as a call does`() {
        assertEquals(
            listOf(
                "IDENTIFIER:page",
                "ASSIGN:=",
                "STATEMENT_KEYWORD:try",
                "STATEMENT_KEYWORD:run",
                "ACTION_NAME:web-fetch",
                "LEFT_PAREN:(",
                "IDENTIFIER:url",
                "COLON::",
                "IDENTIFIER:hit",
                "DOT:.",
                "MEMBER:url",
                "RIGHT_PAREN:)",
                "MODIFIER_KEYWORD:tee",
            ),
            tokens("page = try run web-fetch(url: hit.url) tee"),
        )
    }

    @Test
    fun `a binding may still be called run`() {
        // `run = run x()` is legal -- a statement word before `=` is a name --
        // and the example flows relied on that spelling before `run` existed.
        assertEquals(
            listOf(
                "IDENTIFIER:run",
                "ASSIGN:=",
                "STATEMENT_KEYWORD:run",
                "ACTION_NAME:some-action",
            ),
            tokens("run = run some-action"),
        )
    }

    @Test
    fun `a context carries across a line break`() {
        assertEquals(
            listOf("STATEMENT_KEYWORD:call", "ACTION_NAME:some-action"),
            tokens("call\n  some-action"),
        )
    }

    @Test
    fun `a word is a stage only after a pipe`() {
        assertEquals(
            listOf(
                "IDENTIFIER:page",
                "DOT:.",
                "MEMBER:text",
                "PIPE:|",
                "STAGE:truncate",
                "NUMBER:200",
                "PIPE:|",
                "STAGE:count",
            ),
            tokens("page.text | truncate 200 | count"),
        )
        // The same words, nowhere near a pipe: `text` is the port type it is
        // here, and `count` is nothing in particular.
        assertEquals(
            listOf("TYPE:text", "IDENTIFIER:count"),
            tokens("text count"),
        )
    }

    @Test
    fun `then and where are stages without a pipe, with an operand`() {
        assertEquals(
            listOf(
                "STRING:\"first\"",
                "STAGE:then",
                "IDENTIFIER:shout",
                "DOT:.",
                "MEMBER:upper",
                "STAGE:then",
                "STRING:\"last\"",
            ),
            tokens("""  "first" then shout.upper then "last"  """),
        )
        assertEquals(
            listOf("IDENTIFIER:hits", "STAGE:where", "CONSTANT:it", "DOT:.", "MEMBER:ok"),
            tokens("hits where it.ok"),
        )
        // With nothing to be a stage *of*, both are ordinary names: a port may
        // be called `then`, and this is where the compiler reads one.
        assertEquals(listOf("IDENTIFIER:then"), tokens("then"))
        assertEquals(
            listOf("IDENTIFIER:where", "ARROW:->", "IDENTIFIER:answer"),
            tokens("where -> answer"),
        )
        assertEquals(
            listOf("IDENTIFIER:then", "PIPE:|", "STAGE:count"),
            tokens("then | count"),
        )
    }

    @Test
    fun `node is the keyword only where it makes a node`() {
        assertEquals(
            listOf(
                "IDENTIFIER:said",
                "ASSIGN:=",
                "DECLARATION_KEYWORD:node",
                "LEFT_PAREN:(",
                "RIGHT_PAREN:)",
                "DECLARATION_KEYWORD:in",
                "IDENTIFIER:scratch",
            ),
            tokens("said = node() in scratch"),
        )
        // Making one takes parentheses, so a bare `node` is a name -- a port
        // called that, or the id somebody passed in.
        assertEquals(
            listOf("IDENTIFIER:node", "ARROW:->", "IDENTIFIER:answer"),
            tokens("node -> answer"),
        )
    }

    @Test
    fun `a function is a function where it is called`() {
        assertEquals(
            listOf(
                "STAGE:map",
                "BUILTIN:join",
                "LEFT_PAREN:(",
                "CONSTANT:it",
                "COMMA:,",
                "STRING:\", \"",
                "RIGHT_PAREN:)",
            ),
            tokens("""| map join(it, ", ")""").drop(1),
        )
        // `join` with no argument list after it is the stage, not the function.
        assertEquals(listOf("PIPE:|", "STAGE:join"), tokens("| join"))
    }

    @Test
    fun `a status code is a constant and a field is a field`() {
        assertEquals(
            listOf("STATEMENT_KEYWORD:fail", "STATUS_CODE:not_found", "STRING:\"gone\""),
            tokens("""fail not_found "gone""""),
        )
        assertEquals(
            listOf("STATEMENT_KEYWORD:fail", "STATUS_CODE:NOT_FOUND"),
            tokens("fail NOT_FOUND"),
        )
        // `ok` after a dot is the field of a status record, not the code.
        assertEquals(
            listOf("IDENTIFIER:outcome", "DOT:.", "MEMBER:ok"),
            tokens("outcome.ok"),
        )
    }

    @Test
    fun `a dash joins a name but an arrow does not`() {
        assertEquals(listOf("IDENTIFIER:for-each"), tokens("for-each"))
        assertEquals(
            listOf("IDENTIFIER:a", "ARROW:->", "IDENTIFIER:b"),
            tokens("a -> b"),
        )
        assertEquals(
            listOf("IDENTIFIER:state", "CARRY:<-", "IDENTIFIER:step"),
            tokens("state <- step"),
        )
        assertEquals(listOf("NUMBER:-3"), tokens("-3"))
    }

    @Test
    fun `numbers carry a duration unit, and only a real one`() {
        assertEquals(listOf("DURATION:250ms"), tokens("250ms"))
        assertEquals(listOf("DURATION:1.5h"), tokens("1.5h"))
        assertEquals(listOf("NUMBER:42"), tokens("42"))
        // The compiler rejects an unknown unit, so the editor says so too.
        assertEquals(listOf("BAD_CHARACTER:5x"), tokens("5x"))
    }

    @Test
    fun `a comment runs to the end of its line and a string does not span one`() {
        // `later`, not `after`: `after` is a call modifier, and the lexer is
        // right to say so.
        assertEquals(
            listOf("COMMENT:# a comment", "IDENTIFIER:later"),
            tokens("# a comment\nlater"),
        )
        assertEquals(
            listOf("STRING:\"with \\\" inside\"", "IDENTIFIER:later"),
            tokens("\"with \\\" inside\" later"),
        )
        assertEquals(
            listOf("STRING:\"unterminated", "IDENTIFIER:next"),
            tokens("\"unterminated\nnext"),
        )
    }

    @Test
    fun `the lexer restarts from any token boundary`() {
        // What incremental relexing does: resume mid-file with the state the
        // lexer reported there, and get the same answer.
        val source =
            "in frames: list[a11.NodeFragment] stream\n" +
                "page.text | truncate 200 -> out\ncall some-action"
        val whole = tokens(source)
        val lexer = FlowLexer()
        lexer.start(source, 0, source.length, 0)
        var index = 0
        while (lexer.tokenType != null) {
            val resumed = FlowLexer()
            resumed.start(source, lexer.tokenStart, source.length, lexer.state)
            val rest = mutableListOf<String>()
            while (resumed.tokenType != null) {
                if (resumed.tokenType != TokenType.WHITE_SPACE) {
                    val text = source.substring(resumed.tokenStart, resumed.tokenEnd)
                    rest += "${resumed.tokenType.toString().removePrefix("A11Flow:")}:$text"
                }
                resumed.advance()
            }
            assertEquals("resuming at offset ${lexer.tokenStart}", whole.drop(index), rest)
            if (lexer.tokenType != TokenType.WHITE_SPACE) index++
            lexer.advance()
        }
    }

    @Test
    fun `canonical follows the compiler's casing rule`() {
        assertEquals("for", FlowVocabulary.canonical("for"))
        assertEquals("for", FlowVocabulary.canonical("FOR"))
        assertEquals("For", FlowVocabulary.canonical("For"))
        assertEquals("starts-with", FlowVocabulary.canonical("STARTS-WITH"))
        assertEquals("_", FlowVocabulary.canonical("_"))
    }

    @Test
    fun `a flow is recognised inside a string by what it says`() {
        assertTrue(FlowInjector.looksLikeFlow("flow shout { }"))
        assertTrue(FlowInjector.looksLikeFlow("\n  FLOW shout {\n}\n"))
        assertTrue(FlowInjector.looksLikeFlow("# what this is\nflow a-b {"))
        assertTrue(FlowInjector.looksLikeFlow("""flow "quoted name" {"""))
        assertFalse(FlowInjector.looksLikeFlow("a flow shout { }"))
        assertFalse(FlowInjector.looksLikeFlow("flow shout"))
        assertFalse(FlowInjector.looksLikeFlow("Flow shout { }"))
        assertFalse(FlowInjector.looksLikeFlow("select * from flow"))
        assertFalse(FlowInjector.looksLikeFlow(""))
        assertFalse(FlowInjector.looksLikeFlow("   \n\n  # only comments\n"))
    }
}
