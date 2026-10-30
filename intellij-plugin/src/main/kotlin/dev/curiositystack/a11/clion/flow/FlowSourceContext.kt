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

import com.intellij.lang.injection.InjectedLanguageManager
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.util.Key
import com.intellij.psi.PsiFile

/** Import identity and source snapshots supplied to the native module loader. */
data class FlowSourceContext(
    val path: String? = null,
    val modules: Map<String, String> = emptyMap(),
) {
    internal fun arguments(): Map<String, Any?> = buildMap {
        path?.let { put("path", it) }
        if (modules.isNotEmpty()) put("modules", modules)
    }

    companion object {
        /** A host can attach a more definitive context to a file or injection. */
        val KEY: Key<FlowSourceContext> = Key.create("a11.flow.sourceContext")

        /** Use the physical host file as the base when no explicit context exists. */
        fun fromFile(file: PsiFile): FlowSourceContext {
            file.getUserData(KEY)?.let { return it }
            file.originalFile.getUserData(KEY)?.let { return it }
            val host = InjectedLanguageManager.getInstance(file.project).getTopLevelFile(file)
            host.getUserData(KEY)?.let { return it }
            host.originalFile.getUserData(KEY)?.let { return it }
            val physical = host.originalFile.virtualFile
                ?.takeIf { it.isInLocalFileSystem }
            val path = physical?.canonicalPath ?: physical?.path
            val documents = FileDocumentManager.getInstance()
            val modules = buildMap {
                for (document in documents.unsavedDocuments) {
                    val dependency = documents.getFile(document)
                        ?.takeIf { it.isInLocalFileSystem && it.extension == "flow" }
                        ?: continue
                    val identity = dependency.canonicalPath ?: dependency.path
                    if (identity != path) put(identity, document.text)
                }
            }
            return FlowSourceContext(path, modules)
        }
    }
}
