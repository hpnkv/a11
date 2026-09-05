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

import com.intellij.lang.Language
import com.intellij.openapi.fileTypes.LanguageFileType
import com.intellij.openapi.util.IconLoader
import javax.swing.Icon

/**
 * The A11 Flow language: a composition of A11 actions, written as text.
 *
 * The id is what an injection marker names, so `# language=A11Flow` above a
 * string literal (or `@Language("A11Flow")` on a Java or Kotlin one) injects
 * this language into it. A flow that *looks* like one is injected without any
 * marker at all -- see [FlowInjector].
 */
object FlowLanguage : Language("A11Flow", "text/x-a11flow") {

    private fun readResolve(): Any = FlowLanguage

    override fun getDisplayName(): String = "A11 Flow"

    override fun isCaseSensitive(): Boolean = true
}

/** `.flow` files: one or more `flow` declarations. */
object FlowFileType : LanguageFileType(FlowLanguage) {

    override fun getName(): String = "A11 Flow"

    override fun getDescription(): String = "A11 Flow composition"

    override fun getDefaultExtension(): String = "flow"

    override fun getIcon(): Icon? = FlowIcons.FILE
}

internal object FlowIcons {
    val FILE: Icon? =
        runCatching {
            IconLoader.getIcon("/icons/a11flow.svg", FlowIcons::class.java)
        }.getOrNull()
}
