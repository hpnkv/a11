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

import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import java.awt.Font
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test

class FlowSyntaxHighlighterTest {
    @Test
    fun `native escapes reach the string escape palette`() {
        val highlighter = FlowSyntaxHighlighter()
        assertSame(FlowColors.STRING_ESCAPE, highlighter.getTokenHighlights(FlowTokens.STRING_ESCAPE).single())
        assumeTrue(FlowEngine.instance().available)
        val source = "\"a\\0b\\n\""
        val lexer = FlowLexer()
        lexer.start(source, 0, source.length, 0)
        val escapes = mutableListOf<String>()
        while (lexer.tokenType != null) {
            if (lexer.tokenType == FlowTokens.STRING_ESCAPE) {
                escapes += source.substring(lexer.tokenStart, lexer.tokenEnd)
            }
            lexer.advance()
        }
        assertEquals(listOf("\\0", "\\n"), escapes)
    }

    @Test
    fun `standard types inherit the primitive keyword palette`() {
        val highlighter = FlowSyntaxHighlighter()
        val key = highlighter.getTokenHighlights(FlowTokens.BUILTIN_TYPE).single()
        assertSame(FlowColors.BUILTIN_TYPE, key)
        assertSame(DefaultLanguageHighlighterColors.KEYWORD, key.fallbackAttributeKey)
        assertSame(FlowColors.BUILTIN_TYPE, highlighter.getTokenHighlights(FlowTokens.BUILTIN_TYPE_QUALIFIER).single())
        assertSame(FlowColors.TYPE, highlighter.getTokenHighlights(FlowTokens.TYPE).single())
        assertSame(FlowColors.PUNCTUATION, highlighter.getTokenHighlights(FlowTokens.SCOPE).single())
    }

    @Test
    fun `port style is italic by default`() {
        val key = FlowSyntaxHighlighter().getTokenHighlights(FlowTokens.PORT_NAME).single()
        assertSame(FlowColors.PORT_NAME, key)
        assertTrue(key.defaultAttributes!!.fontType and Font.ITALIC != 0)
    }

    @Test
    fun `native port names and type tokens reach the IDEA palette`() {
        assumeTrue(FlowEngine.instance().available)
        val source = """
            mod text exported {
              flow clean exported {
                in input: string stream
                out output: string stream
                input -> output
              }
            }
            flow main {
              std::duration timeout = 1s
              string start = take arguments
              wait all
            }
        """.trimIndent()
        val lexer = FlowLexer()
        lexer.start(source, 0, source.length, 0)
        val kinds = mutableListOf<String>()
        while (lexer.tokenType != null) {
            kinds += "${lexer.tokenType.toString().removePrefix("A11Flow:")}:${source.substring(lexer.tokenStart, lexer.tokenEnd)}"
            lexer.advance()
        }
        assertEquals(2, kinds.count { it == "PORT_NAME:input" })
        assertEquals(2, kinds.count { it == "PORT_NAME:output" })
        assertTrue(kinds.contains("BUILTIN_TYPE:string"))
        assertTrue(kinds.contains("BUILTIN_TYPE:duration"))
        assertTrue(kinds.contains("STATEMENT_KEYWORD:take"))
        assertTrue(kinds.contains("STATEMENT_KEYWORD:all"))
        assertTrue(kinds.contains("BUILTIN_TYPE:std"))
        assertTrue(kinds.contains("BUILTIN_TYPE_QUALIFIER:::"))
    }
}
