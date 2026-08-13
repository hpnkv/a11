package dev.curiositystack.a11.clion.highlights

import com.intellij.openapi.Disposable
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.EditorKind
import com.intellij.openapi.editor.event.CaretEvent
import com.intellij.openapi.editor.event.CaretListener
import com.intellij.openapi.editor.event.EditorMouseEvent
import com.intellij.openapi.editor.event.EditorMouseEventArea
import com.intellij.openapi.editor.event.EditorMouseListener
import com.intellij.openapi.editor.event.EditorMouseMotionListener
import com.intellij.openapi.editor.event.VisibleAreaEvent
import com.intellij.openapi.editor.event.VisibleAreaListener
import com.intellij.openapi.editor.ex.EditorSettingsExternalizable
import com.intellij.openapi.editor.impl.EditorMouseHoverPopupControl
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.Disposer
import com.intellij.util.Alarm

/** How long a mouse that has left both the range and the popup gets to come back. */
private const val LEAVE_GRACE_MS = 250

/**
 * How long the mouse has to rest on an analysed highlight before the popup opens:
 * whatever the user set for editor tooltips, under Appearance & Behavior.
 *
 * Not a number of our own. Hovering an underline is an established gesture in this
 * IDE and it already has a dwell time; a popup on the same underline that used a
 * different one would feel like a different feature reacting to the same movement.
 */
private fun hoverDelayMs(): Int = EditorSettingsExternalizable.getInstance().tooltipsDelay

/**
 * Opens the suggestion popup when the mouse rests on a range A11 commented on.
 *
 * One listener on the editor factory's multicaster rather than one per editor: the
 * suggestions are keyed by file, any editor may be showing that file, and editors
 * open and close while they stand.
 *
 * That reach is also the trap. The multicaster sees *every* editor in the IDE,
 * including the two inside this feature's own popup, so a listener that closes the
 * popup on a caret move closed it on the diff viewer's own opening scroll: the
 * viewer moves its caret to the first change, this heard it, cancelled the popup,
 * disposed the diff's editors — and the scroll still in flight then died on
 * "Editor is already disposed", which is why the diff never appeared. Hence
 * [watches]: only the user's file editors are events worth acting on.
 *
 * While the mouse is inside a commented range the platform's own hover popup is
 * switched off ([EditorMouseHoverPopupControl]) — not to hide it, but because
 * [SuggestionPopup] renders the same content itself, below the diff. Two popups for
 * one hover is the failure mode this avoids; the disable/enable pair is balanced on
 * every path out, including the editor being closed under the mouse.
 */
internal class SuggestionHover(
    private val project: Project,
    private val suggestions: HighlightSuggestions,
) : Disposable {

    /** Fires the open, and the close; both are "after the mouse stopped doing that". */
    private val alarm = Alarm(Alarm.ThreadToUse.SWING_THREAD, this)

    /** The editor the native popup is currently switched off in, if any. */
    private var suppressedIn: Editor? = null

    init {
        Disposer.register(suggestions, this)
        val multicaster = EditorFactory.getInstance().eventMulticaster
        multicaster.addEditorMouseMotionListener(
            object : EditorMouseMotionListener {
                override fun mouseMoved(event: EditorMouseEvent) = onMouseMoved(event)
            },
            this,
        )
        multicaster.addEditorMouseListener(
            object : EditorMouseListener {
                override fun mouseExited(event: EditorMouseEvent) {
                    if (watches(event.editor)) onMouseLeft(event.editor)
                }

                // A click is the user doing something else; the popup is in the way of it.
                override fun mousePressed(event: EditorMouseEvent) {
                    if (watches(event.editor) && !SuggestionPopup.isMouseOver()) close()
                }
            },
            this,
        )
        // Scrolling or moving the caret moves the text the popup is anchored to.
        multicaster.addVisibleAreaListener(
            object : VisibleAreaListener {
                override fun visibleAreaChanged(event: VisibleAreaEvent) {
                    if (!watches(event.editor)) return
                    if (event.oldRectangle?.location != event.newRectangle.location) close()
                }
            },
            this,
        )
        multicaster.addCaretListener(
            object : CaretListener {
                override fun caretPositionChanged(event: CaretEvent) {
                    if (watches(event.editor)) close()
                }
            },
            this,
        )
    }

    /**
     * Whether this editor's events are any of our business.
     *
     * A main file editor of this project, and nothing else. That rules out consoles
     * and previews, and — the reason this exists — the diff viewer's own editors
     * inside the popup, whose caret and scrolling are the popup drawing itself and
     * must not be read as the user walking away from it.
     */
    private fun watches(editor: Editor): Boolean {
        if (editor.editorKind != EditorKind.MAIN_EDITOR) return false
        val owner = editor.project ?: return false
        return owner == project
    }

    private fun onMouseMoved(event: EditorMouseEvent) {
        val editor = event.editor
        if (!watches(editor)) return
        val suggestion = suggestionUnder(event)
        if (suggestion == null) {
            onMouseLeft(editor)
            return
        }
        // The native popup stays off for as long as the mouse is in here, so that
        // what the user reads is this popup's rendering of it and not both.
        suppress(editor)
        if (SuggestionPopup.isShowingFor(suggestion)) {
            alarm.cancelAllRequests()
            return
        }
        alarm.cancelAllRequests()
        alarm.addRequest({ SuggestionPopup.showAt(project, editor, suggestion, null) }, hoverDelayMs())
    }

    /**
     * The suggestion the pointer is over, or null.
     *
     * Only the text area counts: a hover over the scrollbar, the gutter or the line
     * numbers is not a hover over the code.
     */
    private fun suggestionUnder(event: EditorMouseEvent): Suggestion? {
        if (event.area != EditorMouseEventArea.EDITING_AREA) return null
        val editor = event.editor
        val point = event.mouseEvent.point
        val position = editor.xyToLogicalPosition(point)
        val offset = editor.logicalPositionToOffset(position)
        // `xyToLogicalPosition` happily reports a column past the end of the line —
        // the empty space to the right of the text — and every such column maps to
        // the same offset. Comparing line and column (and not the positions
        // themselves, which also carry `leansForward` and would never be equal) is
        // what tells "on the last character" from "somewhere out to the right of it".
        val actual = editor.offsetToLogicalPosition(offset)
        if (actual.line != position.line || actual.column != position.column) return null
        return suggestions.at(editor.document, offset)
    }

    /** The mouse has left the range (or the editor): close, unless it went into the popup. */
    private fun onMouseLeft(editor: Editor) {
        release(editor)
        alarm.cancelAllRequests()
        alarm.addRequest({ if (!SuggestionPopup.isMouseOver()) SuggestionPopup.hide() }, LEAVE_GRACE_MS)
    }

    private fun close() {
        alarm.cancelAllRequests()
        SuggestionPopup.hide()
    }

    private fun suppress(editor: Editor) {
        if (suppressedIn === editor) return
        suppressedIn?.let { EditorMouseHoverPopupControl.enablePopups(it) }
        EditorMouseHoverPopupControl.disablePopups(editor)
        suppressedIn = editor
    }

    private fun release(editor: Editor) {
        if (suppressedIn !== editor) return
        EditorMouseHoverPopupControl.enablePopups(editor)
        suppressedIn = null
    }

    override fun dispose() {
        suppressedIn?.let { EditorMouseHoverPopupControl.enablePopups(it) }
        suppressedIn = null
        SuggestionPopup.hide()
    }
}
