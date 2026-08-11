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
