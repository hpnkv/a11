package dev.curiositystack.a11.clion.flow

import com.intellij.lang.injection.MultiHostInjector
import com.intellij.lang.injection.MultiHostRegistrar
import com.intellij.psi.ElementManipulators
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiLanguageInjectionHost

/**
 * Highlights a flow written inside somebody else's string literal.
 *
 * Flows are meant to travel as text, so most of the ones a person reads are not
 * in a `.flow` file at all -- they are in a Python triple-quoted string handed
 * to `flow.loads`, in a test, in a Kotlin constant. This is the same treatment
 * SQL gets in a Python string, arrived at the same way: the platform is told
 * that a range inside the host literal is another language, and everything else
 * -- colours, brace matching, comment toggling, *Edit A11 Flow Fragment* --
 * follows from that.
 *
 * The trigger is the content, not a configuration: a string whose first real
 * word is `flow` followed by a `{`, with or without a name in between, is a
 * flow, and nothing else plausibly is. The name is optional because a file's
 * entry point is declared `flow { ... }` and travels in a string as readily as
 * a named one. That makes it work in every IDE, for every host language with a
 * string literal, with nothing to set up. Where the content cannot say so --
 * a fragment, or a flow assembled from pieces -- IntelliLang's own markers still
 * apply, because this language is registered like any other:
 *
 * ```python
 * # language=A11Flow
 * fragment = "shout.upper | first 1 -> loudest"
 * ```
 *
 * and `@Language("A11Flow")` does the same for a Java or Kotlin host.
 */
class FlowInjector : MultiHostInjector {

    override fun elementsToInjectIn(): List<Class<out PsiElement>> =
        listOf(PsiLanguageInjectionHost::class.java)

    override fun getLanguagesToInject(
        registrar: MultiHostRegistrar,
        context: PsiElement,
    ) {
        if (context !is PsiLanguageInjectionHost || !context.isValidHost) return
        // A flow inside a flow is just a flow.
        if (context.language === FlowLanguage) return
        // Cheap rejections first: this runs for every string literal in every
        // file of every language, so it has to cost nothing to say no.
        if (context.textLength !in MINIMUM_LENGTH..MAXIMUM_LENGTH) return
        if (!context.textContains('{')) return

        val range = ElementManipulators.getValueTextRange(context)
        if (range.isEmpty) return
        val value = range.subSequence(context.text)
        if (!looksLikeFlow(value)) return

        registrar
            .startInjecting(FlowLanguage)
            .addPlace(null, null, context, range)
            .doneInjecting()
    }

    companion object {
        /** `flow{}` is the shortest thing worth looking at. */
        private const val MINIMUM_LENGTH = 6

        /** Past this, a string literal is not a flow somebody is reading. */
        private const val MAXIMUM_LENGTH = 200_000

        /**
         * `flow NAME {`, and `flow {` for a file's unnamed entry point.
         *
         * The name is one group, optional as a whole: dropping the `\s+` with
         * it is what keeps `flowing {` from reading as an unnamed flow.
         */
        private val DECLARATION = Regex(
            """(?:flow|FLOW)(?:\s+(?:[A-Za-z_$][A-Za-z0-9_$]*(?:-[A-Za-z0-9_$]+)*|"[^"\n]{1,200}"))?\s*\{""",
        )

        /**
         * Whether ``text`` opens with a flow declaration.
         *
         * Leading blank lines and `#` comments are skipped, because a string
         * that starts with a line explaining what the flow is for is still a
         * flow. Anything else in front of the declaration means this is prose
         * that mentions flows rather than a flow.
         */
        fun looksLikeFlow(text: CharSequence): Boolean {
            var index = 0
            while (index < text.length) {
                val char = text[index]
                when {
                    char.isWhitespace() -> index++
                    char == '#' -> {
                        while (index < text.length && text[index] != '\n') {
                            index++
                        }
                    }

                    else -> return DECLARATION.matchesAt(text, index)
                }
            }
            return false
        }
    }
}
