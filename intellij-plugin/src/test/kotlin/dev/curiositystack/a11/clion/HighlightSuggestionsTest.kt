package dev.curiositystack.a11.clion

import com.intellij.openapi.command.WriteCommandAction
import com.intellij.openapi.command.undo.UndoManager
import com.intellij.openapi.editor.Document
import com.intellij.openapi.editor.impl.DocumentMarkupModel
import com.intellij.openapi.editor.markup.EffectType
import com.intellij.openapi.editor.markup.HighlighterLayer
import com.intellij.openapi.fileEditor.FileEditorManager
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.curiositystack.a11.clion.highlights.HighlightNote
import dev.curiositystack.a11.clion.highlights.HighlightSuggestions
import dev.curiositystack.a11.clion.highlights.Origin
import dev.curiositystack.a11.clion.tools.Patch

/**
 * The editor end of **Suggest fixes**, against a real (headless) platform fixture:
 * recording what the model said about a range, marking that range, assembling one
 * suggestion out of the two records the flow streams for it, finding it again by
 * offset, and the patch machinery the popup's diff and Apply button are built on.
 *
 * The popup itself is not exercised here. It is Swing on the EDT holding a diff
 * viewer, and a headless fixture that showed it would be testing the platform's
 * popup framework rather than anything this plugin decides. What *is* testable is
 * everything the popup asks before it draws: is the range still there, does the
 * patch still fit, what would applying it do.
 */
class HighlightSuggestionsTest : BasePlatformTestCase() {

    fun testSuggestMarksTheRangeAndFindsItByOffset() {
        myFixture.configureByText("marked.txt", "alpha\nbeta\ngamma\n")
        val document = myFixture.editor.document
        val suggestions = HighlightSuggestions.getInstance(project)

        // Line 1, columns 0..4: the whole of "beta".
        val suggestion = suggestions.suggest(note(startLine = 1, endLine = 1, endColumn = 4))

        val beta = document.text.indexOf("beta")
        assertEquals("beta", document.getText(suggestion.range!!))
        assertSame(suggestion, suggestions.at(document, beta))
        assertSame(suggestion, suggestions.at(document, beta + 3))
        // The line above is not what the comment is about.
        assertNull(suggestions.at(document, 0))

        // And the range is marked in the document's own markup, so every editor
        // showing the file has it — that marker is what a hover finds.
        val markers = a11Markers(document)
        assertEquals(1, markers.size)
        assertEquals(beta, markers.single().startOffset)
    }

    fun testAZeroWidthHighlightIsWidenedToSomethingHoverable() {
        myFixture.configureByText("empty-range.txt", "alpha\nbeta\n")
        val suggestions = HighlightSuggestions.getInstance(project)

        // Start and end at the same place: a highlight on a position, not a span.
        val suggestion = suggestions.suggest(note(startLine = 1, endLine = 1, startColumn = 0, endColumn = 0))

        assertEquals("beta", myFixture.editor.document.getText(suggestion.range!!))
    }

    fun testTheRangeFollowsAnEditAboveIt() {
        myFixture.configureByText("moving.txt", "alpha\nbeta\ngamma\n")
        val document = myFixture.editor.document
        val suggestions = HighlightSuggestions.getInstance(project)
        val suggestion = suggestions.suggest(note(startLine = 2, endLine = 2, endColumn = 5))
        assertEquals("gamma", document.getText(suggestion.range!!))

        WriteCommandAction.runWriteCommandAction(project) { document.insertString(0, "inserted\n") }

        // A comment is about a piece of text, not about a line number: applying one
        // suggestion is how the user reaches the next, so the marks have to move.
        assertEquals("gamma", document.getText(suggestion.range!!))
        assertSame(suggestion, suggestions.at(document, document.text.indexOf("gamma")))
    }

    fun testClearAllForgetsEverythingAndUnmarksIt() {
        myFixture.configureByText("cleared.txt", "alpha\nbeta\n")
        val document = myFixture.editor.document
        val suggestions = HighlightSuggestions.getInstance(project)
        suggestions.suggest(note(startLine = 0, endLine = 0, endColumn = 5))
        suggestions.suggest(note(startLine = 1, endLine = 1, endColumn = 4))
        assertEquals(2, suggestions.all().size)

        suggestions.clearAll()

        assertEquals(0, suggestions.all().size)
        assertNull(suggestions.at(document, 0))
        assertTrue(a11Markers(document).isEmpty())
    }

    fun testDroppingASuggestionTakesItsMarkWithIt() {
        myFixture.configureByText("dropped.txt", "alpha\nbeta\n")
        val document = myFixture.editor.document
        val suggestions = HighlightSuggestions.getInstance(project)
        val suggestion = suggestions.suggest(note(startLine = 1, endLine = 1, endColumn = 4))

        suggestions.drop(suggestion)

        assertNull(suggestions.at(document, document.text.indexOf("beta")))
        assertEquals(0, suggestions.all().size)
    }

    fun testAFoundRangeIsUnderlinedAndAReportedOneIsNot() {
        myFixture.configureByText("origins.txt", "alpha\nbeta\ngamma\n")
        val document = myFixture.editor.document
        val suggestions = HighlightSuggestions.getInstance(project)

        // A reported range already carries the daemon's own squiggle, and this
        // marker sits above every severity it draws: painting an effect here would
        // erase the warning the note is about.
        suggestions.suggest(note(startLine = 1, endLine = 1, endColumn = 4))
        // A found range has no squiggle to protect and nothing else saying it is
        // there, so it gets an underline of its own — and not one of the daemon's.
        suggestions.suggest(note(startLine = 2, endLine = 2, endColumn = 5, origin = Origin.FOUND))

        val markers = a11Markers(document).sortedBy { it.startOffset }
        assertEquals(2, markers.size)
        assertNull(markers.first().getTextAttributes(null))
        val underline = markers.last().getTextAttributes(null)
        assertNotNull("a found range should paint its own underline", underline)
        assertEquals(EffectType.BOLD_DOTTED_LINE, underline!!.effectType)
        assertNotNull(underline.effectColor)
        // Only the underline: a found range is not recoloured or reboxed.
        assertNull(underline.foregroundColor)
        assertNull(underline.backgroundColor)
    }

    fun testACommentAndItsPatchAreOneSuggestion() {
        myFixture.configureByText("merged.txt", "alpha\nbeta\ngamma\n")
        val document = myFixture.editor.document
        val suggestions = HighlightSuggestions.getInstance(project)

        // The two ports of the flow, in the order it asks for: the sentence, then
        // the diff for the same range. One place in the editor, not two.
        val first = suggestions.suggest(note(startLine = 1, endLine = 1, endColumn = 4, id = "s1", comment = "mind the sign"))
        val second = suggestions.suggest(
            note(startLine = 1, endLine = 1, endColumn = 4, id = "s1", comment = "", patch = "@@ -2,1 +2,1 @@\n-beta\n+BETA\n"),
        )

        assertSame("the patch should have merged into the comment's suggestion", first, second)
        assertEquals(1, suggestions.all().size)
        assertEquals(1, a11Markers(document).size)
        assertEquals("mind the sign", first.comment)
        assertTrue(first.patch.isNotEmpty())
    }

    fun testAPatchMayArriveBeforeItsComment() {
        myFixture.configureByText("reordered.txt", "alpha\nbeta\ngamma\n")
        val suggestions = HighlightSuggestions.getInstance(project)

        // The flow asks for the comment first because that is the useful order, but a
        // model that answers the other way round must still make one mark and not two.
        val first = suggestions.suggest(
            note(startLine = 1, endLine = 1, endColumn = 4, comment = "", patch = "@@ -2,1 +2,1 @@\n-beta\n+BETA\n", id = "s1"),
        )
        val second = suggestions.suggest(note(startLine = 2, endLine = 2, endColumn = 5, id = "s1", comment = "and here is why"))

        assertSame(first, second)
        assertEquals(1, suggestions.all().size)
        assertEquals("and here is why", first.comment)
        // The range belongs to whichever record arrived first: the mark is already
        // drawn, and moving it under a popup being read is worse than ignoring a
        // second opinion about where it goes.
        assertEquals("beta", myFixture.editor.document.getText(first.range!!))
    }

    fun testUnkeyedRecordsDoNotMergeIntoEachOther() {
        myFixture.configureByText("unkeyed.txt", "alpha\nbeta\ngamma\n")
        val suggestions = HighlightSuggestions.getInstance(project)

        // No id is the flow saying "this record stands alone", not "match it to
        // whatever else came without one".
        suggestions.suggest(note(startLine = 1, endLine = 1, endColumn = 4, comment = "one"))
        suggestions.suggest(note(startLine = 1, endLine = 1, endColumn = 4, comment = "two"))

        assertEquals(2, suggestions.all().size)
    }

    fun testMergingNeverBlanksWhatIsAlreadyThere() {
        myFixture.configureByText("kept.txt", "alpha\nbeta\n")
        val suggestions = HighlightSuggestions.getInstance(project)
        val first = suggestions.suggest(note(startLine = 1, endLine = 1, endColumn = 4, id = "s1", comment = "keep me"))

        // A patch record carries no comment. That is the absence of a second half,
        // not a retraction of the first.
        suggestions.suggest(
            note(startLine = 1, endLine = 1, endColumn = 4, id = "s1", comment = "", patch = "@@ -2,1 +2,1 @@\n-beta\n+BETA\n"),
        )

        assertEquals("keep me", first.comment)
    }

    fun testFromJsonReadsTheIdWhateverItsType() {
        fun id(vararg fields: Pair<String, Any?>) =
            HighlightNote.fromJson(mapOf("path" to "a.txt", "comment" to "hello", *fields)).id

        assertEquals("s1", id("id" to "s1"))
        assertEquals("s1", id("id" to "  s1  "))
        // The flow asks for a short string and a model writes a bare number often
        // enough; either is a usable key, so neither is refused.
        assertEquals("7", id("id" to 7))
        // No id at all is legal: the record stands alone.
        assertEquals("", id())
    }

    fun testFromJsonReadsWhereTheRangeCameFrom() {
        fun origin(vararg fields: Pair<String, Any?>) =
            HighlightNote.fromJson(mapOf("path" to "a.txt", "patch" to "@@ -1,1 +1,1 @@", *fields)).origin

        assertEquals(Origin.FOUND, origin("origin" to "found"))
        assertEquals(Origin.FOUND, origin("origin" to "FOUND"))
        // Anything else is the conservative kind, which paints nothing over the
        // daemon's work: a mangled origin should not put an A11 underline on a
        // range the IDE is already underlining itself.
        assertEquals(Origin.REPORTED, origin())
        assertEquals(Origin.REPORTED, origin("origin" to "reported"))
        assertEquals(Origin.REPORTED, origin("origin" to "found by me, actually"))
        assertEquals(Origin.REPORTED, origin("origin" to 7))
    }

    fun testAFoundRangeMayCarryOnlyAPatch() {
        // The point of the found kind: a repetitive fix worth applying and not worth
        // a sentence at each occurrence.
        val note = HighlightNote.fromJson(
            mapOf(
                "path" to "a.txt",
                "origin" to "found",
                "patch" to "@@ -3,1 +3,1 @@\n-  int x;\n+  int x = 0;",
            ),
        )
        assertEquals(Origin.FOUND, note.origin)
        assertEquals("", note.comment)
        assertTrue(note.patch.isNotEmpty())
    }

    fun testANoteNeedsSomethingToSay() {
        // Neither a comment nor a patch: there would be nothing in the popup.
        assertThrows {
            HighlightNote.fromJson(mapOf("path" to "a.txt", "comment" to "  ", "patch" to ""))
        }
        assertThrows { HighlightNote.fromJson(mapOf("comment" to "say something")) }

        // Either one alone is enough.
        assertEquals("mind the sign", HighlightNote.fromJson(mapOf("path" to "a.txt", "comment" to "mind the sign")).comment)
        assertEquals("@@ -1,1 +1,1 @@", HighlightNote.fromJson(mapOf("path" to "a.txt", "patch" to "@@ -1,1 +1,1 @@")).patch)
    }

    fun testASkipCommentIsNotAComment() {
        // A model told to leave the field out sometimes writes the word instead.
        // Showing "SKIP" in a popup is worse than showing no popup.
        assertThrows { HighlightNote.fromJson(mapOf("path" to "a.txt", "comment" to "skip")) }

        // But it does not throw away a patch that came with it.
        val note = HighlightNote.fromJson(
            mapOf("path" to "a.txt", "comment" to "SKIP - nothing to add", "patch" to "@@ -1,1 +1,1 @@\n-a\n+b\n"),
        )
        assertEquals("", note.comment)
        assertTrue(note.patch.isNotEmpty())
    }

    fun testPreviewShowsOnlyWhatThePatchTouches() {
        // No trailing newline, as the other patch tests here write their fixtures:
        // whether a final break makes an extra empty line is the document's business
        // and not what this asserts.
        myFixture.configureByText("previewed.txt", (1..20).joinToString("\n") { "line $it" })
        val document = myFixture.editor.document
        val edits = Patch.locate(document, "@@ -10,1 +10,1 @@\n-line 10\n+LINE TEN\n")

        val preview = Patch.preview(document, edits)

        // Two lines of context either side of the one changed line — enough to place
        // it in a popup that sits over the file itself.
        assertEquals(7, preview.firstLine)
        assertEquals(
            listOf("line 8", "line 9", "line 10", "line 11", "line 12"),
            preview.before.split("\n"),
        )
        assertEquals(
            listOf("line 8", "line 9", "LINE TEN", "line 11", "line 12"),
            preview.after.split("\n"),
        )
    }

    fun testPreviewIsAUnifiedSequenceOfKeptRemovedAndAddedLines() {
        myFixture.configureByText("unified.txt", (1..8).joinToString("\n") { "line $it" })
        val document = myFixture.editor.document

        val preview = Patch.preview(document, Patch.locate(document, "@@ -4,1 +4,2 @@\n-line 4\n+FIRST\n+SECOND\n"))

        // This is what the popup paints: one column of lines, each saying whether it
        // is being left alone, taken out, or put in — removed before added, so the
        // change reads top to bottom.
        assertEquals(
            listOf(
                Patch.Kind.KEPT to "line 2",
                Patch.Kind.KEPT to "line 3",
                Patch.Kind.REMOVED to "line 4",
                Patch.Kind.ADDED to "FIRST",
                Patch.Kind.ADDED to "SECOND",
                Patch.Kind.KEPT to "line 5",
                Patch.Kind.KEPT to "line 6",
            ),
            preview.lines.map { it.kind to it.text },
        )
        // A removed line is the file's own text, not the patch's copy of it, so the
        // two derived views stay consistent with the file.
        assertEquals("line 2\nline 3\nline 4\nline 5\nline 6", preview.before)
        assertEquals("line 2\nline 3\nFIRST\nSECOND\nline 5\nline 6", preview.after)
    }

    fun testPreviewCoversAddedAndRemovedLines() {
        myFixture.configureByText("grown.txt", "one\ntwo\nthree")
        val document = myFixture.editor.document

        val added = Patch.preview(document, Patch.locate(document, "@@ -2,1 +2,2 @@\n two\n+extra\n"))
        assertEquals(listOf("one", "two", "extra", "three"), added.after.split("\n"))
        assertEquals(listOf("one", "two", "three"), added.before.split("\n"))

        val removed = Patch.preview(document, Patch.locate(document, "@@ -2,1 +2,0 @@\n-two\n"))
        assertEquals(listOf("one", "three"), removed.after.split("\n"))
    }

    fun testLocateRefusesAPatchThatDoesNotFit() {
        myFixture.configureByText("mismatched.txt", "alpha\nbeta\n")
        val document = myFixture.editor.document

        // This is what makes "if there is a valid patch" decidable: the popup asks
        // the same code that would apply it, and shows no diff when it says no.
        assertThrows { Patch.locate(document, "@@ -1,1 +1,1 @@\n-not in the file\n+something\n") }
        assertThrows { Patch.locate(document, "there is no hunk here") }
    }

    fun testApplyEditsTheDocumentAsOneUndoableCommand() {
        myFixture.configureByText("applied.txt", "alpha\nbeta\ngamma\n")
        val document = myFixture.editor.document
        val edits = Patch.locate(document, "@@ -2,1 +2,1 @@\n-beta\n+BETA\n")

        Patch.apply(project, document, edits)
        assertEquals("alpha\nBETA\ngamma\n", document.text)

        // The popup's Apply button and `apply_patch` land in the same command, so
        // one Ctrl+Z takes either of them back.
        val undo = UndoManager.getInstance(project)
        val editor = FileEditorManager.getInstance(project).getSelectedEditor(myFixture.file.virtualFile)
        assertTrue("an applied suggestion should be undoable", undo.isUndoAvailable(editor))
        undo.undo(editor)
        assertEquals("alpha\nbeta\ngamma\n", document.text)
    }

    override fun tearDown() {
        try {
            HighlightSuggestions.getInstance(project).clearAll()
        } finally {
            super.tearDown()
        }
    }

    /**
     * A11's own markers in a document's markup, by the layer it puts them on.
     *
     * There is nothing else to recognise them by: the marker for a reported range
     * paints no attributes (the daemon's squiggle is the underline) and carries no
     * gutter icon, since hovering the range is the whole affordance.
     */
    private fun a11Markers(document: Document) =
        DocumentMarkupModel.forDocument(document, project, false)!!
            .allHighlighters
            .filter { it.layer == HighlighterLayer.LAST + 1 }

    /** One record the flow would send for the open file, at the given 0-based range. */
    private fun note(
        startLine: Int,
        endLine: Int,
        startColumn: Int = 0,
        endColumn: Int = 0,
        comment: String = "Say something about this.",
        patch: String = "",
        origin: Origin = Origin.REPORTED,
        id: String = "",
    ) = HighlightNote(
        path = myFixture.file.virtualFile.path,
        comment = comment,
        patch = patch,
        startLine = startLine,
        startColumn = startColumn,
        endLine = endLine,
        endColumn = endColumn,
        origin = origin,
        id = id,
    )

    private fun assertThrows(block: () -> Unit) {
        try {
            block()
        } catch (expected: IllegalArgumentException) {
            return
        }
        fail("expected the call to be refused")
    }
}
