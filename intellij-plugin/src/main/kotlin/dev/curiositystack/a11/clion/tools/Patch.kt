package dev.curiositystack.a11.clion.tools

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.command.CommandProcessor
import com.intellij.openapi.editor.Document
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.TextRange
import com.intellij.psi.PsiDocumentManager

/** What the IDE calls the patch in its Undo menu, and how it groups it. */
private const val PATCH_COMMAND_NAME = "Apply Patch (A11)"
private const val PATCH_COMMAND_GROUP = "a11.apply_patch"

/**
 * The `@@ -before,count +after,count @@` line that opens a hunk.
 *
 * Only the first number is read, and the rest of the header is not checked. It is
 * advisory — a hunk is placed by its context — so a header that is mangled but
 * recognisable (`@@ -5,1,5,1 @@`, which a model will write) should still open the
 * hunk it announces rather than make the whole patch unreadable.
 */
private val HUNK_HEADER = Regex("""@@+\s*-(\d+)[^@]*@@.*""")

/**
 * How much unchanged context a preview shows around what a patch touches.
 *
 * Two lines, not the three a `diff` writes: this is read in a popup over the code
 * it is about, where the file itself is the context, so the lines either side are
 * there to place the change and not to explain it.
 */
private const val PREVIEW_CONTEXT_LINES = 2

/**
 * Reading a unified diff, placing it against a document, and applying it.
 *
 * Its own object rather than a corner of [IdeTools] because two callers need the
 * same answers from it. `apply_patch` reads a patch, places it, and applies it in
 * one go for a model that is not watching. The editor's suggestion popup places
 * the same patch *without* applying it — twice: once to decide whether there is a
 * diff worth showing at all, and again when the button is pressed, because the
 * file may have moved in between. "A valid patch" therefore means exactly what
 * the tool means by it, decided by this code and not by the model that wrote it.
 */
internal object Patch {

    /**
     * Every hunk of [patch] placed against [document], top to bottom.
     *
     * Located by context rather than by the line numbers in the `@@` headers, and
     * each hunk has to match: a patch that does not fit is refused with the text
     * that was there instead. That is the only safe answer for an edit nobody is
     * watching — a fuzzy match is how a tool silently rewrites the wrong lines.
     *
     * A patch whose markers are indented — the other slip a written-by-hand diff
     * makes — is read again that way rather than refused, and the file is still
     * what decides: if neither reading matches, the first one's report is the
     * honest one, because it is the format that was asked for.
     *
     * @throws IllegalArgumentException if there are no hunks, or one does not fit.
     */
    fun locate(document: Document, patch: String): List<Applied> {
        val hunks = parseHunks(patch, indented = false)
        require(hunks.isNotEmpty()) {
            "No hunks in the patch: expected a unified diff, with a line per change" +
                " prefixed ' ' to keep, '-' to remove or '+' to add."
        }
        return try {
            locateAll(document, hunks)
        } catch (mismatch: IllegalArgumentException) {
            val indented = parseHunks(patch, indented = true)
            try {
                locateAll(document, indented)
            } catch (again: IllegalArgumentException) {
                throw mismatch
            }
        }
    }

    /**
     * Apply already-placed [edits] to [document] as a single undoable command.
     *
     * One command, so one Undo takes it back — the same reversibility a rename
     * has, and for the same reason: the IDE's own undo stack, not a copy of the
     * file kept somewhere. The document is saved after, because a patch to a file
     * nobody has open would otherwise sit in memory looking applied.
     *
     * Applied bottom to top, so every offset [locate] worked out is still the
     * offset it was: an edit above would have moved the ones below it. Call on the
     * EDT; the write action is taken here.
     */
    fun apply(project: Project, document: Document, edits: List<Applied>) {
        CommandProcessor.getInstance().executeCommand(
            project,
            {
                ApplicationManager.getApplication().runWriteAction {
                    for (edit in edits.asReversed()) rewrite(document, edit)
                }
            },
            PATCH_COMMAND_NAME,
            PATCH_COMMAND_GROUP,
        )
        PsiDocumentManager.getInstance(project).commitDocument(document)
        FileDocumentManager.getInstance().saveDocument(document)
    }

    /**
     * What [edits] would do, as a unified sequence of lines: kept, removed, added.
     *
     * The window is what the patch touches plus [context] lines either side, not
     * the whole file: a diff of a 2000-line file scrolled to one changed line is a
     * worse answer than the changed line with its neighbours, and this is read in
     * a popup.
     *
     * Unified rather than two texts side by side, because one column of lines is
     * what fits where this is shown, and because the marker each line carries is
     * enough to colour it. Kept and removed lines are the *document's* own text
     * rather than the patch's copy of them (see [matchAt]), so what the reader sees
     * on the left of a change is what is really in the file.
     */
    fun preview(document: Document, edits: List<Applied>, context: Int = PREVIEW_CONTEXT_LINES): Preview {
        val lineCount = document.lineCount
        if (lineCount == 0 || edits.isEmpty()) return Preview(emptyList(), 0)
        val ordered = edits.sortedBy { it.at }
        val from = (ordered.first().at - context).coerceAtLeast(0)
        val past = ordered.maxOf { it.at + it.hunk.before.size }
        val to = (past + context).coerceAtMost(lineCount)

        val lines = ArrayList<Line>()
        var line = from
        var next = 0
        while (line < to) {
            val edit = ordered.getOrNull(next)?.takeIf { it.at == line }
            if (edit != null) {
                // A hunk that only adds matches no lines, so the line it was placed
                // at is still ahead of us and is written out on the next turn.
                line = expand(document, edit, lines)
                next += 1
                continue
            }
            lines.add(Line(Kind.KEPT, lineText(document, line)))
            line += 1
        }
        // A hunk that only adds, placed past the last line, is an append: the loop
        // above never reaches the line it sits at, because there is not one.
        while (next < ordered.size) {
            expand(document, ordered[next], lines)
            next += 1
        }
        return Preview(lines, from)
    }

    /**
     * Write one placed hunk out as unified lines; returns the document line to
     * carry on from, which every kept and removed line advances by one.
     */
    private fun expand(document: Document, edit: Applied, into: MutableList<Line>): Int {
        var at = edit.at
        for (line in edit.hunk.lines) {
            when (line.kind) {
                '+' -> into.add(Line(Kind.ADDED, line.text))
                '-' -> into.add(Line(Kind.REMOVED, lineText(document, at++)))
                else -> into.add(Line(Kind.KEPT, lineText(document, at++)))
            }
        }
        return at
    }

    /** What a previewed line is: left alone, taken out, or put in. */
    enum class Kind { KEPT, REMOVED, ADDED }

    /** One line of a preview. */
    class Line(val kind: Kind, val text: String)

    /**
     * The lines a patch touches, in unified order, and the 0-based file line the
     * window starts at.
     */
    class Preview(val lines: List<Line>, val firstLine: Int) {
        /** The window as the file has it now — everything the patch did not add. */
        val before: String get() = joined { it != Kind.ADDED }

        /** And as the patch would leave it — everything it did not remove. */
        val after: String get() = joined { it != Kind.REMOVED }

        private fun joined(keep: (Kind) -> Boolean): String =
            lines.filter { keep(it.kind) }.joinToString("\n") { it.text }
    }

    /**
     * One hunk, the 0-based line it was found at, and the lines to leave there.
     *
     * The replacement is worked out where the match was, not where the hunk was
     * read: a context line is the *file's* line, whichever of the two spellings
     * of it the patch used (see [matchAt]).
     */
    class Applied(val hunk: Hunk, val at: Int, val replacement: List<String>)

    /** How many lines the placed [edits] leave behind, and how many they replace. */
    fun added(edits: List<Applied>): Int = edits.sumOf { it.hunk.after.size }

    fun removed(edits: List<Applied>): Int = edits.sumOf { it.hunk.before.size }

    /** Every hunk placed against the file, top to bottom, or the first mismatch. */
    private fun locateAll(document: Document, hunks: List<Hunk>): List<Applied> {
        val edits = ArrayList<Applied>(hunks.size)
        var searchFrom = 0
        for (hunk in hunks) {
            val edit = locateHunk(document, hunk, searchFrom)
            edits.add(edit)
            searchFrom = edit.at + hunk.before.size
        }
        return edits
    }

    /** Replace the lines a hunk matched with the ones it carries. */
    private fun rewrite(document: Document, edit: Applied) {
        val start = document.getLineStartOffset(edit.at)
        val replacement = edit.replacement.joinToString("\n")
        if (edit.hunk.before.isEmpty()) {
            // Nothing to replace: a hunk that only adds is an insertion above the
            // line it was placed at, and it brings its own line break.
            document.insertString(start, replacement + "\n")
            return
        }
        val past = edit.at + edit.hunk.before.size
        if (edit.hunk.after.isEmpty()) {
            // A hunk that only removes takes the lines' break with them, or a
            // deleted line would leave an empty one where it was.
            val end = if (past < document.lineCount) document.getLineStartOffset(past) else document.textLength
            document.deleteString(start, end)
            return
        }
        document.replaceString(start, document.getLineEndOffset(past - 1), replacement)
    }

    /**
     * Where [hunk] fits in [document], searching from [searchFrom] downwards.
     *
     * The `@@` header's line is tried first when it has one and it is at or after
     * where the last hunk left off, because a patch generated against this very
     * file will land there and the scan is then a formality.
     */
    private fun locateHunk(document: Document, hunk: Hunk, searchFrom: Int): Applied {
        val lines = document.lineCount
        if (hunk.before.isEmpty()) {
            val at = hunk.hintLine ?: throw IllegalArgumentException(
                "A hunk that only adds lines needs an '@@' header to say where they go.",
            )
            require(at in 0..lines) { "The hunk at line ${at + 1} is past the end of the file." }
            return Applied(hunk, at.coerceIn(searchFrom, lines.coerceAtLeast(0)), hunk.added())
        }
        val hinted = hunk.hintLine?.takeIf { it >= searchFrom }
        val candidates = (listOfNotNull(hinted) + (searchFrom until lines)).distinct()
        for (start in candidates) {
            if (start + hunk.before.size > lines) continue
            matchAt(document, hunk, start)?.let { return Applied(hunk, start, it) }
        }
        val found = if (hunk.hintLine != null && hunk.hintLine < lines) {
            val to = (hunk.hintLine + hunk.before.size).coerceAtMost(lines)
            (hunk.hintLine until to).joinToString("\n") { lineText(document, it) }
        } else {
            ""
        }
        throw IllegalArgumentException(
            "This hunk does not match the file, so nothing was applied:\n\n" +
                hunk.before.joinToString("\n") +
                (if (found.isEmpty()) "" else "\n\nWhat is at line ${hunk.hintLine!! + 1} instead:\n\n$found") +
                "\n\nRead the file again and patch what is there.",
        )
    }

    /**
     * The lines to leave at [start] if [hunk] matches the file there, else null.
     *
     * Two spellings of every kept and removed line are tried: the one the diff's
     * prefix implies, and the raw line as written. That is not fuzz — it is the
     * one mistake in a hand- or model-written patch that the file itself can
     * settle. A line of an indented file *starts* with a space, so a patch that
     * leaves the ' ' prefix off a context line is indistinguishable from one that
     * includes it and means a line indented one space less; only the file knows
     * which, and here it says. Whichever spelling matched is what goes back,
     * because a kept line is the file's line and not the patch's copy of it.
     */
    private fun matchAt(document: Document, hunk: Hunk, start: Int): List<String>? {
        val kept = ArrayList<String>(hunk.before.size)
        for ((offset, line) in hunk.beforeLines().withIndex()) {
            val actual = lineText(document, start + offset)
            if (!sameLine(line.text, actual) && !sameLine(line.raw, actual)) return null
            kept.add(actual)
        }
        val replacement = ArrayList<String>(hunk.after.size)
        var taken = 0
        for (line in hunk.lines) {
            when (line.kind) {
                '+' -> replacement.add(line.text)
                '-' -> taken += 1
                else -> replacement.add(kept[taken++])
            }
        }
        return replacement
    }

    private fun lineText(document: Document, line: Int): String = document.getText(
        TextRange(document.getLineStartOffset(line), document.getLineEndOffset(line)),
    )

    /**
     * Whether a patch line and a file line are the same line.
     *
     * Trailing whitespace is ignored, and only trailing: a diff that has been
     * through a chat window, a JSON string or an editor that strips it should
     * still apply, while indentation — which is meaning, in more than one language
     * — has to be exactly right.
     */
    private fun sameLine(expected: String, actual: String): Boolean =
        expected.trimEnd() == actual.trimEnd()

    /**
     * Every hunk of a unified diff, in order.
     *
     * File headers are skipped rather than checked: the file is a separate input,
     * so a `---`/`+++` pair says nothing this call does not already know, and a
     * patch generated for one path should still apply to the one it was sent with.
     */
    private fun parseHunks(patch: String, indented: Boolean): List<Hunk> {
        val hunks = ArrayList<Hunk>()
        var current: MutableHunk? = null
        // A patch that ends with a newline does not have an empty last line: that
        // break belongs to the line before it. Keeping it would add a phantom
        // context line, which is a hunk that matches somewhere else entirely.
        val lines = patch.split("\n").let { if (it.lastOrNull()?.isEmpty() == true) it.dropLast(1) else it }
        for (raw in lines) {
            val header = HUNK_HEADER.matchEntire(raw.trim())
            if (header != null) {
                current?.let { hunks.add(it.build()) }
                // 1-based in the format, 0-based here; a hunk header of 0 means an
                // empty file, which is line 0 either way.
                current = MutableHunk((header.groupValues[1].toInt() - 1).coerceAtLeast(0))
                continue
            }
            val hunk = current ?: continue
            // With `indented`, whatever whitespace was put in front of the marker
            // goes with it; without, the first character is the marker, which is
            // what a unified diff says.
            val line = if (indented) raw.trimStart(' ', '\t') else raw
            when {
                line.startsWith("+") -> hunk.lines.add(PatchLine('+', line.substring(1), line))
                line.startsWith("-") -> hunk.lines.add(PatchLine('-', line.substring(1), line))
                raw.startsWith(" ") -> hunk.lines.add(PatchLine(' ', raw.substring(1), raw))
                // "\ No newline at end of file" says something about the last line,
                // not a line of its own.
                raw.startsWith("\\") -> Unit
                // A bare empty line is how an unchanged empty line survives a trip
                // through anything that trims. Anything else ends the hunk.
                raw.isEmpty() -> hunk.lines.add(PatchLine(' ', "", ""))
                else -> {
                    hunks.add(hunk.build())
                    current = null
                }
            }
        }
        current?.let { hunks.add(it.build()) }
        return hunks
    }

    /** A hunk being read out of a patch. */
    private class MutableHunk(val hintLine: Int) {
        val lines = ArrayList<PatchLine>()

        fun build(): Hunk = Hunk(lines.toList(), hintLine)
    }

    /**
     * One line of a hunk: what it does, and the two ways of reading it.
     *
     * [text] is the line with its prefix taken off, [raw] the line as the patch
     * wrote it. They differ by one character, and which one is the file's line is
     * a question only the file can answer — see [matchAt].
     */
    class PatchLine(val kind: Char, val text: String, val raw: String)

    /** One hunk: its lines in order, and the line its header points at. */
    class Hunk(val lines: List<PatchLine>, val hintLine: Int?) {
        /** The lines it expects to find, kept and removed alike, in order. */
        fun beforeLines(): List<PatchLine> = lines.filter { it.kind != '+' }

        /** The same, as text; for counting and for reporting a mismatch. */
        val before: List<String> get() = beforeLines().map { it.text }

        /** What it leaves behind, as text; for counting. */
        val after: List<String> get() = lines.filter { it.kind != '-' }.map { it.text }

        /** Just the added lines, for a hunk with nothing to match. */
        fun added(): List<String> = lines.filter { it.kind == '+' }.map { it.text }
    }
}
