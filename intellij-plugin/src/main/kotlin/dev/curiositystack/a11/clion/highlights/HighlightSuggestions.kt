package dev.curiositystack.a11.clion.highlights

import com.intellij.openapi.Disposable
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.ReadAction
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import com.intellij.openapi.editor.Document
import com.intellij.openapi.editor.impl.DocumentMarkupModel
import com.intellij.openapi.editor.markup.EffectType
import com.intellij.openapi.editor.markup.HighlighterLayer
import com.intellij.openapi.editor.markup.HighlighterTargetArea
import com.intellij.openapi.editor.markup.RangeHighlighter
import com.intellij.openapi.editor.markup.TextAttributes
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.Disposer
import com.intellij.openapi.util.TextRange
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.ui.JBColor
import dev.curiositystack.a11.clion.tools.ProjectFiles
import dev.curiositystack.a11.clion.tools.TargetFile
import java.awt.Color
import java.awt.Font

/**
 * A review suggestion attached to a current file range.
 *
 * [RangeHighlighter] tracks the range as edits move the surrounding text.
 *
 * [comment] and [patch] may arrive independently on separate Flow output ports.
 * [id] correlates them.
 */
class Suggestion internal constructor(
    val file: VirtualFile,
    /**
     * Correlation key for a comment and patch that may arrive in either order.
     *
     * Empty is legal and means "no correlation" — such a record stands alone,
     * and a second one never merges into it.
     */
    val id: String,
    /**
     * Whether the IDE reported this range, or
     * the model found it in the file itself.
     */
    val origin: Origin,
    comment: String,
    patch: String,
    internal val highlighter: RangeHighlighter,
) {
    /** Review text for the popup. Empty until it arrives. */
    var comment: String = comment
        internal set

    /** A unified diff accepted by `apply_patch`. Empty until it arrives. */
    var patch: String = patch
        internal set

    val path: String get() = file.path

    /** Where it applies now, or null once the text it was about has gone. */
    val range: TextRange?
        get() = if (highlighter.isValid) TextRange(highlighter.startOffset, highlighter.endOffset) else null
}

/**
 * Stores review comments and patches on their document ranges.
 *
 * **Suggest fixes** sends the file and IDE warnings to a review flow. Returned
 * suggestions may address a warning or identify another range. Markers use the
 * document markup model, so all editors for the file share them and they
 * survive closing and reopening a tab. [SuggestionHover] opens
 * [SuggestionPopup].
 *
 * [suggest] combines comment and patch records by [Suggestion.id], allowing the
 * range to appear before patch generation completes.
 *
 * IDE-reported ranges retain their existing underline. A11 underlines ranges
 * found only by the model; see [Origin].
 *
 * Suggestions are not persisted. Each flow run replaces the current set, and
 * closing the project clears it.
 *
 * This light service is registered by [Service]. It must not also appear as a
 * `<projectService>` in `plugin.xml`, because duplicate registration fails
 * descriptor loading.
 */
@Service(Service.Level.PROJECT)
class HighlightSuggestions(private val project: Project) : Disposable {

    /** Suggestions in arrival order, keyed by absolute path. */
    private val byPath = LinkedHashMap<String, MutableList<Suggestion>>()

    /** The hover listener, installed on the first suggestion and not before. */
    private var hover: SuggestionHover? = null

    /**
     * Record one Flow note or merge it into an existing suggestion.
     *
     * The first record creates and marks the suggestion. A later record with
     * the same [HighlightNote.id] fills the missing comment or patch,
     * regardless of arrival order.
     *
     * The first record defines the range and origin. Later correlated records
     * do not move an existing marker.
     *
     * Input ranges use zero-based lines and columns. Clamp them because the
     * document may have changed during review and model-provided ranges may be
     * invalid.
     */
    fun suggest(note: HighlightNote): Suggestion = onEdt {
        val target = ReadAction.compute<TargetFile, RuntimeException> { ProjectFiles.resolve(project, note.path) }
        existing(target.path, note.id)?.let { return@onEdt merge(it, note) }

        val document = target.document
        val range = clampToDocument(document, note)
        val markup = DocumentMarkupModel.forDocument(document, project, true)
        val highlighter = markup.addRangeHighlighter(
            range.startOffset,
            range.endOffset,
            SUGGESTION_LAYER,
            underlineFor(note.origin),
            HighlighterTargetArea.EXACT_RANGE,
        )
        val suggestion = Suggestion(target.file, note.id, note.origin, note.comment, note.patch, highlighter)
        byPath.getOrPut(target.path) { ArrayList() }.add(suggestion)
        if (hover == null) hover = SuggestionHover(project, this)
        suggestion
    }

    /**
     * The live suggestion [id] names in [path], or null.
     *
     * A scan of the file's own list rather than a second map keyed by id: the
     * list is a handful of entries long, and an index would be one more thing
     * for [drop] and [clear] to keep in step. An empty id never matches, so an
     * unkeyed record always gets a suggestion of its own.
     */
    private fun existing(path: String, id: String): Suggestion? {
        if (id.isEmpty()) return null
        return byPath[path]?.firstOrNull { it.id == id && it.range != null }
    }

    /**
     * Fill in what [note] brings and [suggestion] does not yet have.
     *
     * Only ever adds: a second record for the same id is the other half of the
     * suggestion, not a correction of it, so a field that already has text is
     * left alone rather than overwritten by an empty one.
     */
    private fun merge(suggestion: Suggestion, note: HighlightNote): Suggestion {
        if (note.comment.isNotEmpty()) suggestion.comment = note.comment
        if (note.patch.isNotEmpty()) suggestion.patch = note.patch
        // The user may already be hovering the range this was marked on,
        // reading a comment whose diff has just this moment arrived.
        SuggestionPopup.refreshFor(suggestion)
        return suggestion
    }

    /** The suggestion covering [offset] in [document], or null. */
    fun at(document: Document, offset: Int): Suggestion? {
        val path = document.file()?.path ?: return null
        return byPath[path]?.lastOrNull { suggestion ->
            suggestion.range?.let { offset >= it.startOffset && offset <= it.endOffset } == true
        }
    }

    /**
     * Every suggestion still standing, for a caller that wants to count them.
     */
    fun all(): List<Suggestion> = byPath.values.flatten().filter { it.range != null }

    /** Forget one suggestion — it has been applied, or it no longer fits. */
    fun drop(suggestion: Suggestion) = onEdt {
        suggestion.highlighter.dispose()
        val list = byPath[suggestion.path] ?: return@onEdt
        list.remove(suggestion)
        if (list.isEmpty()) byPath.remove(suggestion.path)
    }

    /**
     * Forget everything: what one run of the
     * flow left, before the next one starts.
     */
    fun clearAll() = onEdt {
        for (list in byPath.values) for (suggestion in list) suggestion.highlighter.dispose()
        byPath.clear()
    }

    /** Forget one file's suggestions. */
    fun clear(path: String) = onEdt {
        val resolved = runCatching {
            ReadAction.compute<String, RuntimeException> { ProjectFiles.resolve(project, path).path }
        }.getOrDefault(path)
        byPath.remove(resolved)?.forEach { it.highlighter.dispose() }
    }

    override fun dispose() {
        clearAll()
        // Disposed here rather than left to the tree: it holds an alarm and
        // four listeners on the editor factory's multicaster, which outlives
        // the project.
        hover?.let { Disposer.dispose(it) }
        hover = null
    }

    /**
     * The requested range, as offsets this document actually has.
     *
     * `end_column` is exclusive in what the
     * IDE reported, so it is an offset as it
     * stands. An empty range — which is what a highlight on a zero-width
     * position comes back as — is widened to the rest of its line, because a
     * marker nobody can put the mouse on is not a marker.
     */
    private fun clampToDocument(document: Document, note: HighlightNote): TextRange {
        val lastLine = (document.lineCount - 1).coerceAtLeast(0)
        fun offsetOf(line: Int, column: Int): Int {
            val onLine = line.coerceIn(0, lastLine)
            val start = document.getLineStartOffset(onLine)
            val end = document.getLineEndOffset(onLine)
            return (start + column.coerceAtLeast(0)).coerceIn(start, end)
        }
        val from = offsetOf(note.startLine, note.startColumn)
        val to = offsetOf(note.endLine, note.endColumn).coerceAtLeast(from)
        if (to > from) return TextRange(from, to)
        val lineEnd = document.getLineEndOffset(note.startLine.coerceIn(0, lastLine))
        return TextRange(from, lineEnd.coerceAtLeast(from))
    }

    /**
     * Run [block] on the EDT and wait;
     * markup and gutter belong to the UI thread.
     */
    private fun <T> onEdt(block: () -> T): T {
        var result: Result<T>? = null
        ApplicationManager.getApplication().invokeAndWait { result = runCatching(block) }
        return result!!.getOrThrow()
    }

    companion object {
        /**
         * Above every severity the daemon draws, so the marker is not painted
         * over by the very warning it is about.
         */
        private const val SUGGESTION_LAYER = HighlighterLayer.LAST + 1

        /**
         * How a range is drawn, which is a different answer for each [Origin].
         *
         * Only the topmost layer's effect is painted, and this marker is above
         * daemon highlights. A [Origin.REPORTED] range therefore has no effect
         * of its own, preserving the daemon's error or warning underline. The
         * range marker still tracks edits and provides the hover target.
         *
         * A range found by the model has no existing diagnostic underline. Draw
         * it with the plugin's blue dotted style to distinguish a suggestion
         * from an IDE error or warning.
         */
        private fun underlineFor(origin: Origin): TextAttributes? = when (origin) {
            Origin.REPORTED -> null
            Origin.FOUND -> TextAttributes(null, null, FOUND_UNDERLINE, EffectType.BOLD_DOTTED_LINE, Font.PLAIN)
        }

        /**
         * The blue of the plugin's own icon:
         * for a light theme, then for a dark one.
         */
        private val FOUND_UNDERLINE = JBColor(Color(0x31, 0x5F, 0xC0), Color(0x54, 0x8A, 0xF7))

        fun getInstance(project: Project): HighlightSuggestions = project.service()
    }
}

/**
 * Where a suggestion's range came from, which decides how it is drawn.
 *
 * [REPORTED] is a range the IDE's own analysis underlined and the flow asked
 * about, so its numbers are `get_error_highlights`' own. [FOUND] is a range the
 * model selected from the file without an IDE diagnostic, so its coordinates
 * come from the model and only this plugin marks the range.
 */
enum class Origin {
    REPORTED,
    FOUND,
}

/**
 * One record as it arrives from the page: the file, a range of it, and either
 * what to say about that range or the patch that fixes it.
 *
 * One suggestion is normally two of these — the flow streams comments and
 * patches on separate ports so the short one need not wait for the long one —
 * tied together by [id]. Carrying both fields at once is still legal and is
 * what a single record with no counterpart looks like.
 *
 * Lines and columns are 0-based, as `get_error_highlights` reports them and as
 * the flow passes them on; `end_column` is exclusive.
 */
data class HighlightNote(
    val path: String,
    val comment: String,
    val patch: String,
    val startLine: Int,
    val startColumn: Int,
    val endLine: Int,
    val endColumn: Int,
    val origin: Origin = Origin.REPORTED,
    /**
     * What the flow called the suggestion this record belongs to; empty when it
     * named none, which means the record stands alone. See
     * [HighlightSuggestions.suggest].
     */
    val id: String = "",
) {
    companion object {
        /**
         * Read a note out of the JSON the bridge was handed.
         *
         * Both `comment` and `patch` are optional and default to empty, because
         * a highlight the model could explain but not fix and one it could fix
         * without having anything to add are both worth a popup — but a note
         * with neither is refused, since there would be nothing in it to show.
         *
         * A comment that is a refusal marker is not a comment. The flow asks
         * the model to leave the field out when it has nothing to add, and a
         * model that writes "SKIP" instead has done the same thing in words —
         * showing that word in a popup would be strictly worse than showing no
         * popup. The check is here, at the edge where the model's text becomes
         * the IDE's UI, rather than in the flow: the language has no
         * conditional expression, so the flow could only drop the whole note,
         * patch and all.
         */
        fun fromJson(json: Map<String, Any?>): HighlightNote {
            fun text(field: String): String = (json[field] as? String).orEmpty().trim()
            fun line(field: String): Int {
                val value = json[field] ?: return 0
                require(value is Number) { "'$field' must be a number." }
                return value.toInt().coerceAtLeast(0)
            }
            val path = json["path"] as? String
            require(!path.isNullOrBlank()) { "A highlight note needs a 'path'." }
            val comment = text("comment").takeUnless { it.uppercase().startsWith("SKIP") }.orEmpty()
            val patch = text("patch")
            require(comment.isNotEmpty() || patch.isNotEmpty()) {
                "A highlight note needs a 'comment', a 'patch', or both."
            }
            val startLine = line("start_line")
            return HighlightNote(
                path = path,
                // Whatever it is, as text: the flow asks the model for a short
                // string and gets a bare number often enough, and either is a
                // usable key.
                id = json["id"]?.toString()?.trim().orEmpty(),
                comment = comment,
                patch = patch,
                startLine = startLine,
                startColumn = line("start_column"),
                endLine = line("end_line").coerceAtLeast(startLine),
                endColumn = line("end_column"),
                // Anything but the model's own word for it reads as a reported
                // range: a note whose origin was mangled is better drawn as the
                // conservative kind, which paints nothing over the daemon's
                // work.
                origin = if (text("origin").equals("found", ignoreCase = true)) Origin.FOUND else Origin.REPORTED,
            )
        }
    }
}

/** The file a document's text came from, when it came from one. */
internal fun Document.file(): VirtualFile? = FileDocumentManager.getInstance().getFile(this)
