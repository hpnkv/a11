package dev.curiositystack.a11.clion.ui

import com.intellij.openapi.project.Project
import com.intellij.openapi.wm.ToolWindow
import com.intellij.openapi.wm.ToolWindowFactory
import com.intellij.ui.components.JBLabel
import com.intellij.ui.jcef.JBCefApp
import com.intellij.util.ui.JBUI
import javax.swing.JComponent
import javax.swing.SwingConstants

/**
 * Registers the "A11 Chat" tool window as a JCEF page. The page runs the
 * TypeScript A11 library, which owns the WebSocket to the Python backend; IDE
 * tools are reached through the JS↔Kotlin bridge in [A11WebView].
 */
class ChatToolWindowFactory : ToolWindowFactory {
    override fun createToolWindowContent(project: Project, toolWindow: ToolWindow) {
        mountA11WebView(project, toolWindow, "chat")
    }
}

/** Registers the "A11 Actions" tool window (the action explorer) on the same bundle. */
class ActionsToolWindowFactory : ToolWindowFactory {
    override fun createToolWindowContent(project: Project, toolWindow: ToolWindow) {
        mountA11WebView(project, toolWindow, "actions")
    }
}

/** Mount an [A11WebView] into [toolWindow], or a fallback message if JCEF is unavailable. */
private fun mountA11WebView(project: Project, toolWindow: ToolWindow, view: String) {
    val component: JComponent =
        if (JBCefApp.isSupported()) {
            A11WebView(project, view, toolWindow.disposable).component
        } else {
            JBLabel(
                "<html><div style='padding:16px'>A11 requires JCEF (the embedded browser), " +
                    "which is not available in this IDE runtime.<br>Enable it via " +
                    "<b>Help → Find Action → Choose Boot Java Runtime</b> and pick a JBR with JCEF.</div></html>",
            ).apply {
                verticalAlignment = SwingConstants.TOP
                border = JBUI.Borders.empty(8)
            }
        }
    val content = toolWindow.contentManager.factory.createContent(component, "", false)
    toolWindow.contentManager.addContent(content)
}
