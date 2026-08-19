package dev.curiositystack.a11.clion.flow

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The indent Enter lands on, inside a flow.
 *
 * Pure-function tests of [indentAfter] rather than a platform fixture: what it
 * answers depends only on the punctuation before the caret, which is exactly
 * what [FlowShape] reports with no tool and no PSI, and testing it that way
 * costs a map lookup rather than a sandboxed IDE.
 */
class FlowEnterHandlerTest {

    private val unit = "  "

    @Test
    fun `an ordinary line inside a block keeps the block's own indent`() {
        val before = "flow f {\n  in a: string"
        assertEquals("  ", indentAfter(before, unit))
    }

    @Test
    fun `a trailing comma indents one level deeper`() {
        val before = "flow f {\n  skip a,"
        assertEquals("    ", indentAfter(before, unit))
    }

    @Test
    fun `a running continuation keeps its own width rather than adding another level`() {
        // The second line is already a continuation, four spaces deep though the
        // block itself is only two; a third line matches it rather than reading
        // it as "the statement's first line" and adding a level on top.
        val before = "flow f {\n  skip a,\n    b,"
        assertEquals("    ", indentAfter(before, unit))
    }

    @Test
    fun `a manually deeper continuation is followed rather than reset`() {
        val before = "flow f {\n  skip a,\n      b,"
        assertEquals("      ", indentAfter(before, unit))
    }

    @Test
    fun `the line after a continuation ends snaps back to the block`() {
        val before = "flow f {\n  skip a,\n    b\n  \"x\" -> out"
        assertEquals("  ", indentAfter(before, unit))
    }

    @Test
    fun `a pipe left open continues the pipeline`() {
        val before = "flow f {\n  words | map strformat(\"x\", it) |"
        assertEquals("    ", indentAfter(before, unit))
    }

    @Test
    fun `an arrow left open continues onto its target`() {
        val before = "flow f {\n  words ->"
        assertEquals("    ", indentAfter(before, unit))
    }

    @Test
    fun `an open parenthesis continues until it closes`() {
        val before = "flow f {\n  skip (o1, o2"
        assertEquals("    ", indentAfter(before, unit))
    }

    @Test
    fun `closing the parenthesis on its own line still continues if a comma follows`() {
        val before = "flow f {\n  skip (o1, o2) of act,"
        assertEquals("    ", indentAfter(before, unit))
    }

    @Test
    fun `a nested block indents one level past its own`() {
        val before = "flow f {\n  for hit in hits {"
        assertEquals("    ", indentAfter(before, unit))
    }

    @Test
    fun `leaving a nested block returns to its own level`() {
        val before = "flow f {\n  for hit in hits {\n    hit -> out\n  }"
        assertEquals("  ", indentAfter(before, unit))
    }

    @Test
    fun `a comment does not itself count as a continuation`() {
        val before = "flow f {\n  \"x\" -> out # done"
        assertEquals("  ", indentAfter(before, unit))
    }

    @Test
    fun `a comma inside a string is not a continuation`() {
        val before = "flow f {\n  \"a, b\" -> out"
        assertEquals("  ", indentAfter(before, unit))
    }

    @Test
    fun `an unterminated multi-line string is left to the default`() {
        val before = "flow f {\n  describe \"\"\"\n  still writing"
        assertNull(indentAfter(before, unit))
    }

    @Test
    fun `a closed triple-quoted string is an ordinary line again`() {
        val before = "flow f {\n  describe \"\"\"\n  two lines\n  \"\"\""
        assertEquals("  ", indentAfter(before, unit))
    }

    @Test
    fun `the request's own combined example`() {
        val before = "flow f {\n" +
            "  act1 = run action1(text: our_input)\n" +
            "  skip our_input,\n" +
            "    act1,\n" +
            "    (o1, o2) of act2,"
        assertEquals("    ", indentAfter(before, unit))
    }
}
