package dev.curiositystack.a11.clion.highlights

import com.intellij.codeInsight.daemon.impl.DaemonCodeAnalyzerEx
import com.intellij.codeInsight.hint.HintUtil
import com.intellij.lang.annotation.HighlightSeverity
import com.intellij.openapi.Disposable
import com.intellij.openapi.diagnostic.thisLogger
import com.intellij.openapi.diff.DiffColors
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.EditorKind
import com.intellij.openapi.editor.colors.EditorColorsManager
import com.intellij.openapi.editor.ex.EditorEx
import com.intellij.openapi.editor.highlighter.EditorHighlighterFactory
import com.intellij.openapi.editor.markup.HighlighterLayer
import com.intellij.openapi.editor.markup.HighlighterTargetArea
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.popup.JBPopup
import com.intellij.openapi.ui.popup.JBPopupFactory
import com.intellij.openapi.ui.popup.JBPopupListener
import com.intellij.openapi.ui.popup.LightweightWindowEvent
import com.intellij.openapi.util.Disposer
import com.intellij.openapi.util.TextRange
import com.intellij.ui.awt.RelativePoint
import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBScrollPane
import com.intellij.util.ui.JBUI
import com.intellij.util.ui.UIUtil
import com.intellij.xml.util.XmlStringUtil
import dev.curiositystack.a11.clion.tools.Patch
import java.awt.Color
import java.awt.Dimension
import java.awt.Point
import javax.swing.Box
import javax.swing.BoxLayout
import javax.swing.JButton
import javax.swing.JComponent
import javax.swing.JPanel
import javax.swing.ScrollPaneConstants

/**
 * How wide the popup is, and how tall it may grow before it scrolls.
 *
 * Sized like a tooltip rather than like a window: the diff inside is a handful of
 * lines and gets exactly the height those lines need, so this is the only cap.
 */
private const val POPUP_WIDTH = 520
private const val POPUP_MAX_HEIGHT = 440

/**
 * The popup on an analysed highlight: what A11 said about it, the fix it proposed,
 * a button that applies the fix, and the IDE's own message underneath.
 *
 * That order is the point of it. The IDE already told the user *what* is wrong —
 * that is the highlight — so this leads with what it could not tell them: why, and
 * what to do about it. The patch is shown as a diff rather than described, because
 * a diff of three lines is read faster than a sentence about three lines, and it
 * is applied by a button rather than by hand, because the plugin can already do it
 * (`apply_patch`) and the user retyping it is pure loss.
 *
 * The IDE's own tooltip is rendered at the bottom rather than replaced. Hovering an
 * underline has one meaning in a JetBrains IDE and it is not "see what the AI
 * thinks" — a popup that swallowed the platform's message would take away the
 * reason the user hovered. So the hover listener suppresses the native popup only
 * to render its content here (see [SuggestionHover]).
 *
 * Only one of these is up at a time; opening a second cancels the first.
 */
internal object SuggestionPopup {

    /** The popup currently on screen, if any. */
    private var showing: JBPopup? = null

    /** Which suggestion it is about, so the hover does not reopen the same one. */
    private var showingFor: Suggestion? = null

    /** Where it was opened, so [refreshFor] can put the replacement in the same place. */
    private var showingIn: Editor? = null
    private var showingProject: Project? = null
    private var showingAt: Point? = null

    /**
     * Show the popup for [suggestion] in [editor], at [at] or at the range itself.
     *
     * Returns null when there is nothing to show — the text the suggestion was
     * about has been edited away, so the comment is now about nothing.
     */
    fun showAt(project: Project, editor: Editor, suggestion: Suggestion, at: Point?): JBPopup? {
        val range = suggestion.range ?: return null
        hide()

        // Disposed with the popup: the diff panel holds editors, and an editor that
        // outlives the window showing it is a leak the platform asserts on.
        val disposable = Disposer.newDisposable("A11 suggestion popup")
        val content = buildContent(project, editor, suggestion, range, disposable)
        val popup = JBPopupFactory.getInstance()
            .createComponentPopupBuilder(content, null)
            .setTitle(title(editor, suggestion, range))
            .setResizable(true)
            .setMovable(true)
            // Not focused, because it opened on a hover: taking the caret away from
            // the editor because the mouse crossed a warning would be hostile.
            .setRequestFocus(false)
            .setFocusable(true)
            .setCancelOnClickOutside(true)
            // No mouse-out cancel callback is installed on purpose: the user has to
            // be able to walk the pointer off the warning and into the popup to
            // press the button. Leaving the range is handled by [SuggestionHover],
            // which checks whether the mouse landed in here first.
            .setCancelOnOtherWindowOpen(true)
            .setCancelKeyEnabled(true)
            .createPopup()
        Disposer.register(popup, disposable)
        popup.addListener(object : JBPopupListener {
            override fun onClosed(event: LightweightWindowEvent) {
                if (showing === popup) forget()
            }
        })

        showing = popup
        showingFor = suggestion
        showingIn = editor
        showingProject = project
        val where = at ?: editor.offsetToXY(range.startOffset).also { it.y += editor.lineHeight }
        showingAt = where
        popup.show(RelativePoint(editor.contentComponent, where))
        return popup
    }

    /**
     * Redraw the popup for [suggestion], in place, if that is the one on screen.
     *
     * The case this exists for: the user is hovering a range whose comment has arrived
     * and is reading it while the flow is still writing the patch for it. When the
     * patch lands the popup on screen is out of date by one diff and an Apply button,
     * and it would stay that way until the mouse left the range and came back.
     *
     * Rebuilt rather than patched in place, because the content is assembled in one
     * pass from what the suggestion holds ([buildContent]) and rebuilding it is what
     * makes "the popup shows what has arrived" true without a second code path. Reopened
     * at exactly the point it was opened at, so the window does not move under the mouse.
     */
    fun refreshFor(suggestion: Suggestion) {
        if (!isShowingFor(suggestion)) return
        // Read before showAt, which begins by calling hide() and clearing all three.
        val project = showingProject ?: return
        val editor = showingIn ?: return
        val at = showingAt
        showAt(project, editor, suggestion, at)
    }

    /** Close whatever is showing. */
    fun hide() {
        showing?.cancel()
        forget()
    }

    private fun forget() {
        showing = null
        showingFor = null
        showingIn = null
        showingProject = null
        showingAt = null
    }

    /** Whether [suggestion] is the one already on screen. */
    fun isShowingFor(suggestion: Suggestion): Boolean =
        showing?.isVisible == true && showingFor === suggestion

    /**
     * Whether the mouse is inside the popup.
     *
     * This is what stops the popup from closing as the user moves the mouse off the
     * warning and towards the Apply button: leaving the range is only a reason to
     * close if the pointer did not land here instead.
     */
    fun isMouseOver(): Boolean {
        val popup = showing?.takeIf { it.isVisible } ?: return false
        val component = popup.content
        return component.isShowing && component.mousePosition != null
    }

    /**
     * `A11 · line 42`, so the popup says which line it is about — and `· found` when
     * the range is one the model picked out rather than one the IDE reported, since
     * there is no warning here for the reader to attribute this to.
     */
    private fun title(editor: Editor, suggestion: Suggestion, range: TextRange): String {
        val line = editor.document.getLineNumber(range.startOffset) + 1
        val found = if (suggestion.origin == Origin.FOUND) " · found" else ""
        return "A11 · line $line$found"
    }

    private fun buildContent(
        project: Project,
        editor: Editor,
        suggestion: Suggestion,
        range: TextRange,
        disposable: Disposable,
    ): JComponent {
        // One background for the whole popup, taken from the same place the native
        // tooltip label below takes its own: `HintUtil.getInformationColor()`. The
        // diff is painted on it too, so the popup reads as one surface rather than
        // as a tooltip with an editor pasted into the middle of it.
        val background = HintUtil.getInformationColor()

        val column = JPanel()
        column.layout = BoxLayout(column, BoxLayout.Y_AXIS)
        column.border = JBUI.Borders.empty(6, 9)
        column.background = background

        if (suggestion.comment.isNotEmpty()) {
            column.add(paragraph(XmlStringUtil.escapeString(suggestion.comment)))
        }

        // Placed against the document as it stands now, not as it stood when the
        // model wrote the patch: "a valid patch" is one that still fits, and it is
        // this code — the same code that would apply it — that decides.
        val edits = if (suggestion.patch.isEmpty()) {
            null
        } else {
            runCatching { Patch.locate(editor.document, suggestion.patch) }.getOrNull()
        }

        val problem = JBLabel()
        problem.foreground = UIUtil.getErrorForeground()
        problem.isVisible = false

        if (edits != null) {
            column.add(Box.createVerticalStrut(6))
            column.add(diffView(project, editor, suggestion, edits, background, disposable))
            column.add(Box.createVerticalStrut(6))
            column.add(applyRow(project, editor, suggestion, problem))
            column.add(problem)
        } else if (suggestion.patch.isNotEmpty()) {
            // There was a patch and it no longer fits. Worth one line: the comment
            // above may well be about a fix the user can no longer take for free.
            column.add(Box.createVerticalStrut(4))
            column.add(paragraph("<i>A11 proposed an edit here, but it no longer matches the file.</i>"))
        }

        nativeTooltip(project, editor, range)?.let { html ->
            column.add(Box.createVerticalStrut(8))
            // Its own component, because this is the platform's rendering of the
            // platform's message — links and all — and not ours to restyle. It
            // already paints on `getInformationColor()`, which is why the rest of
            // the popup was given that colour rather than the other way round.
            column.add(HintUtil.createInformationLabel(html).also { it.alignmentX = 0f })
        }

        val scroll = JBScrollPane(column)
        scroll.border = JBUI.Borders.empty()
        scroll.viewport.background = column.background
        scroll.horizontalScrollBarPolicy = ScrollPaneConstants.HORIZONTAL_SCROLLBAR_NEVER
        val height = column.preferredSize.height.coerceAtMost(POPUP_MAX_HEIGHT)
        scroll.preferredSize = Dimension(POPUP_WIDTH, height)
        return scroll
    }

    /** A wrapped paragraph of HTML at the popup's width. */
    private fun paragraph(html: String): JComponent {
        val label = JBLabel("<html><body style='width:${POPUP_WIDTH - 40}px'>$html</body></html>")
        label.alignmentX = 0f
        return label
    }

    /**
     * The patch as one compact unified diff of the lines it touches.
     *
     * A read-only viewer editor rather than the platform's `DiffRequestPanel`. The
     * full diff viewer is the right component for a diff *window* — two editors,
     * two gutters, two scrollbars and a toolbar — and all of that in a hover popup
     * is chrome around four lines of code, on its own background, looking like a
     * window that fell into a tooltip. This is the same thing the intention preview
     * does: one column, the file's own syntax highlighting, added and removed lines
     * carrying the theme's own diff colours, and the tooltip's background
     * everywhere so the diff is part of the popup rather than a panel inside it.
     */
    private fun diffView(
        project: Project,
        editor: Editor,
        suggestion: Suggestion,
        edits: List<Patch.Applied>,
        background: Color,
        disposable: Disposable,
    ): JComponent {
        val preview = Patch.preview(editor.document, edits)
        val factory = EditorFactory.getInstance()
        val document = factory.createDocument(preview.lines.joinToString("\n") { it.text })
        val pane = factory.createViewer(document, project, EditorKind.PREVIEW) as EditorEx
        // The editor holds native resources and must go when the popup does.
        Disposer.register(disposable) { factory.releaseEditor(pane) }

        pane.setBackgroundColor(background)
        pane.setBorder(JBUI.Borders.empty())
        // Renderer mode: no caret, no selection, nothing to interact with. This is a
        // picture of an edit, and clicking into it would only take the focus.
        pane.isRendererMode = true
        pane.setHorizontalScrollbarVisible(false)
        pane.setVerticalScrollbarVisible(false)
        pane.setCaretEnabled(false)
        pane.settings.apply {
            isLineNumbersShown = false
            isLineMarkerAreaShown = false
            isIndentGuidesShown = false
            isFoldingOutlineShown = false
            isRightMarginShown = false
            isCaretRowShown = false
            isBlinkCaret = false
            isUseSoftWraps = false
            additionalLinesCount = 0
            additionalColumnsCount = 0
        }
        // The gutter is empty and would still paint a stripe of a different colour
        // down the left edge of the popup.
        pane.gutterComponentEx.isPaintBackground = false
        pane.gutterComponentEx.isVisible = false
        // The file's own highlighting, so the snippet reads as the language it is.
        pane.highlighter = EditorHighlighterFactory.getInstance()
            .createEditorHighlighter(project, suggestion.file.fileType)

        paintDiffColours(pane, preview.lines)

        // Exactly as tall as the lines it holds: the popup's own scroll pane is what
        // deals with a patch too long for the screen, so there is no second
        // scrollable area for the wheel to fight over.
        val component = pane.component
        component.preferredSize = Dimension(POPUP_WIDTH - 32, pane.lineHeight * preview.lines.size.coerceAtLeast(1))
        component.alignmentX = 0f
        return component
    }

    /**
     * Colour the added and removed lines the way this theme colours a diff.
     *
     * `LINES_IN_RANGE`, so the colour runs the full width of the popup rather than
     * stopping at the end of each line's text — a ragged right edge is what makes a
     * block of coloured lines read as highlighting instead of as a diff. Taken from
     * the scheme's own `DIFF_INSERTED`/`DIFF_DELETED`, so a user who has themed
     * their diffs sees those colours here too.
     */
    private fun paintDiffColours(pane: EditorEx, lines: List<Patch.Line>) {
        val scheme = EditorColorsManager.getInstance().globalScheme
        val colours = mapOf(
            Patch.Kind.ADDED to scheme.getAttributes(DiffColors.DIFF_INSERTED),
            Patch.Kind.REMOVED to scheme.getAttributes(DiffColors.DIFF_DELETED),
        )
        val document = pane.document
        lines.forEachIndexed { index, line ->
            val attributes = colours[line.kind] ?: return@forEachIndexed
            if (index >= document.lineCount) return@forEachIndexed
            pane.markupModel.addRangeHighlighter(
                document.getLineStartOffset(index),
                document.getLineEndOffset(index),
                HighlighterLayer.SELECTION - 1,
                attributes,
                HighlighterTargetArea.LINES_IN_RANGE,
            )
        }
    }

    /** The Apply button, and what pressing it does. */
    private fun applyRow(
        project: Project,
        editor: Editor,
        suggestion: Suggestion,
        problem: JBLabel,
    ): JComponent {
        val apply = JButton("Apply fix")
        apply.toolTipText = "Apply this patch to the file, as one undoable edit"
        if (!editor.document.isWritable) {
            apply.isEnabled = false
            apply.toolTipText = "This file is not writable."
        }
        apply.addActionListener {
            // Located again, because the file may have changed while the popup was
            // open — including by the user applying the suggestion above this one.
            val edits = runCatching { Patch.locate(editor.document, suggestion.patch) }
            val located = edits.getOrElse { error ->
                problem.text = error.message?.lineSequence()?.firstOrNull() ?: "The patch no longer fits this file."
                problem.isVisible = true
                thisLogger().debug("an A11 suggestion no longer fits its file", error)
                return@addActionListener
            }
            try {
                Patch.apply(project, editor.document, located)
            } catch (error: RuntimeException) {
                problem.text = error.message ?: "The patch could not be applied."
                problem.isVisible = true
                return@addActionListener
            }
            // Applied is done with: the comment was about text that is no longer
            // there, and leaving the mark behind would invite applying it twice.
            HighlightSuggestions.getInstance(project).drop(suggestion)
            hide()
        }
        val row = JPanel()
        row.layout = BoxLayout(row, BoxLayout.X_AXIS)
        row.isOpaque = false
        row.alignmentX = 0f
        row.add(apply)
        row.add(Box.createHorizontalGlue())
        return row
    }

    /**
     * The IDE's own tooltip for the underlines this range overlaps, as one HTML
     * body — or null when the platform had nothing to say beyond the squiggle.
     *
     * Read from the daemon's markup, which is the same place `get_error_highlights`
     * reads and therefore the same text the flow was asked about. Each tooltip
     * arrives as a whole HTML document, so the bodies are unwrapped before being
     * joined; a highlight with no tooltip contributes its plain description.
     */
    private fun nativeTooltip(project: Project, editor: Editor, range: TextRange): String? {
        val bodies = LinkedHashSet<String>()
        DaemonCodeAnalyzerEx.processHighlights(
            editor.document,
            project,
            HighlightSeverity.WARNING,
            range.startOffset,
            range.endOffset,
        ) { info ->
            val tooltip = info.toolTip
                ?: info.description?.takeIf { it.isNotBlank() }?.let { XmlStringUtil.escapeString(it) }
            tooltip?.let { bodies.add(UIUtil.getHtmlBody(it)) }
            true
        }
        val text = bodies.filter { it.isNotBlank() }.joinToString("<br><br>")
        return if (text.isBlank()) null else XmlStringUtil.wrapInHtml(text)
    }
}
