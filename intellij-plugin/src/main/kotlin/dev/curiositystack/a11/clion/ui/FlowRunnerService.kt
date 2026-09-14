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

package dev.curiositystack.a11.clion.ui

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.Disposable
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import com.intellij.openapi.editor.event.DocumentEvent
import com.intellij.openapi.editor.event.DocumentListener
import com.intellij.openapi.project.Project
import com.intellij.openapi.wm.ToolWindowManager
import dev.curiositystack.a11.clion.flow.FlowEngine
import dev.curiositystack.a11.clion.flow.FlowPsiFile

/** Keeps the bottom runner synchronized with the selected native Flow symbol. */
@Service(Service.Level.PROJECT)
class FlowRunnerService(private val project: Project) : Disposable {
    private var view: A11WebView? = null
    private var selected: Selection? = null

    fun open(file: FlowPsiFile, name: String) {
        selected?.dispose()
        val selection = Selection(file, name)
        selected = selection
        selection.refresh()
        ToolWindowManager.getInstance(project).getToolWindow("A11 Flow Runner")?.show()
    }

    fun attach(candidate: A11WebView) {
        view = candidate
        selected?.refresh()
    }

    fun detach(candidate: A11WebView) {
        if (view === candidate) view = null
    }

    override fun dispose() {
        selected?.dispose()
        selected = null
        view = null
    }

    private inner class Selection(private val file: FlowPsiFile, private val name: String) {
        private val listener = object : DocumentListener {
            override fun documentChanged(event: DocumentEvent) {
                ApplicationManager.getApplication().invokeLater { refresh() }
            }
        }

        init {
            file.viewProvider.document?.addDocumentListener(listener)
        }

        fun dispose() {
            file.viewProvider.document?.removeDocumentListener(listener)
        }

        @Suppress("UNCHECKED_CAST")
        fun refresh() {
            if (!file.isValid) return
            val symbols = FlowEngine.instance().symbols(file.text)?.get("symbols") as? List<Map<String, Any?>>
                ?: return
            val symbol = symbols.firstOrNull { it["kind"] == "flow" && it["name"] == name } ?: return
            val selection = symbol["selection"] as? Map<String, Any?> ?: return
            val start = selection["start"] as? Map<String, Any?> ?: return
            val offset = (start["offset"] as? Number)?.toInt() ?: 0
            val document = file.viewProvider.document
            val line = document?.getLineNumber(offset) ?: 0
            val children = symbol["children"] as? List<Map<String, Any?>> ?: emptyList()
            val ports = children.filter { it["kind"] == "port" }.map { port ->
                val detail = (port["detail"] as? String).orEmpty()
                    .split(' ').filter { it.isNotBlank() }
                val modifiers = detail.drop(1)
                linkedMapOf<String, Any?>(
                    "name" to port["name"],
                    "direction" to (port["direction"] ?: detail.firstOrNull()),
                    "type" to (port["type"] ?: modifiers
                        .filter { it != "stream" && it != "required" }
                        .joinToString(" ")),
                    "stream" to (port["stream"] ?: modifiers.contains("stream")),
                    "required" to (port["required"] ?: modifiers.contains("required")),
                )
            }.filter { it["direction"] == "in" || it["direction"] == "out" }
            val tokens = FlowEngine.instance().tokens(file.text)?.get("tokens")
            val flow = linkedMapOf<String, Any?>(
                "source" to file.text,
                "name" to name,
                "path" to (file.virtualFile?.path ?: file.name),
                "line" to line,
                "ports" to ports,
                "tokens" to tokens,
            )
            view?.openFlow(flow)
        }
    }

    companion object {
        fun getInstance(project: Project): FlowRunnerService = project.service()
    }
}
