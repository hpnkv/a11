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

import com.intellij.codeInsight.daemon.LineMarkerInfo
import com.intellij.codeInsight.daemon.LineMarkerProvider
import com.intellij.openapi.util.IconLoader
import com.intellij.openapi.project.DumbAware
import com.intellij.openapi.editor.markup.GutterIconRenderer
import com.intellij.psi.PsiElement
import dev.curiositystack.a11.clion.ui.FlowRunnerService

/** Adds an A11 run affordance at names the native service identifies as flows. */
class FlowRunLineMarkerProvider : LineMarkerProvider, DumbAware {
    @Suppress("UNCHECKED_CAST")
    override fun getLineMarkerInfo(element: PsiElement): LineMarkerInfo<*>? {
        if (element.firstChild != null) return null
        val file = element.containingFile as? FlowPsiFile ?: return null
        val symbols = FlowEngine.instance().symbols(file.text)?.get("symbols") as? List<Map<String, Any?>>
            ?: return null
        val symbol = symbols.firstOrNull { candidate ->
            if (candidate["kind"] != "flow") return@firstOrNull false
            val selection = candidate["selection"] as? Map<String, Any?> ?: return@firstOrNull false
            val start = selection["start"] as? Map<String, Any?> ?: return@firstOrNull false
            val end = selection["end"] as? Map<String, Any?> ?: return@firstOrNull false
            element.textRange.startOffset == (start["offset"] as? Number)?.toInt() &&
                element.textRange.endOffset == (end["offset"] as? Number)?.toInt()
        } ?: return null
        val name = symbol["name"] as? String ?: return null
        return LineMarkerInfo(
            element,
            element.textRange,
            ICON,
            { "Run '$name' with A11" },
            { _, _ -> FlowRunnerService.getInstance(element.project).open(file, name) },
            GutterIconRenderer.Alignment.LEFT,
            { "Run '$name' with A11" },
        )
    }

    companion object {
        private val ICON = IconLoader.getIcon("/icons/a11flow.svg", FlowRunLineMarkerProvider::class.java)
    }
}
