package dev.curiositystack.a11.clion.flow

import com.intellij.codeInsight.CodeInsightSettings
import com.intellij.lang.injection.InjectedLanguageManager
import com.intellij.psi.PsiLanguageInjectionHost
import com.intellij.psi.util.PsiTreeUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase

/**
 * A flow written inside somebody else's string literal is highlighted there.
 *
 * The host here is XML because that is a language every IDE has, and the
 * injector does not care which it is: it is registered for
 * [PsiLanguageInjectionHost] itself, so a Python triple-quoted string, a Kotlin
 * constant and an XML attribute are all the same to it. What it looks at is
 * whether the text is a flow.
 */
class FlowInjectionTest : BasePlatformTestCase() {

    /**
     * Put the code-insight settings back the way they were found.
     *
     * Registering a documentation provider for the language makes the platform
     * turn the javadoc auto-popup on the first time one is asked for, and the
     * fixture's tear-down asserts that a test changed no global setting. The
     * setting is the platform's to change and the assertion is the fixture's to
     * make; restoring it here is what lets both be right.
     */
    override fun tearDown() {
        try {
            // Back to what a fresh instance holds, which is what the fixture
            // compares against: restoring the value read at set-up is not the
            // same thing, since the platform writes the field explicitly the
            // first time it is asked and the snapshot was taken before that.
            CodeInsightSettings.getInstance().AUTO_POPUP_JAVADOC_INFO =
                CodeInsightSettings().AUTO_POPUP_JAVADOC_INFO
        } finally {
            super.tearDown()
        }
    }

    fun testAFlowInsideAStringIsInjected() {
        val injected = injectedLanguageIn(
            """<root note="flow shout { in words: string stream }"/>""",
        )
        assertEquals(FlowLanguage, injected)
    }

    fun testAFlowSpreadOverSeveralLinesIsInjected() {
        val injected = injectedLanguageIn(
            """
            <root note="
              # what this one is for
              flow shout {
                in  words:   string stream
                out loudest: string
                say = call text-upper(text: words)
                say.upper | first 1 -> loudest
              }
            "/>
            """.trimIndent(),
        )
        assertEquals(FlowLanguage, injected)
    }

    fun testProseThatMerelyMentionsAFlowIsNotInjected() {
        assertNull(
            injectedLanguageIn(
                """<root note="the flow shout { } is described below"/>""",
            ),
        )
        assertNull(injectedLanguageIn("""<root note="{not a flow at all}"/>"""))
    }

    fun testTheInjectedFragmentIsHighlightedAsAFlow() {
        myFixture.configureByText(
            "sample.xml",
            """<root note="flow shout { in words: string stream }"/>""",
        )
        val host = hostIn()
        val files = InjectedLanguageManager.getInstance(project)
            .getInjectedPsiFiles(host!!)
        assertNotNull("nothing was injected", files)
        val fragment = files!!.first().first
        assertTrue(fragment is FlowPsiFile)
        // The fragment is the flow, without the quotes around it.
        assertEquals(
            "flow shout { in words: string stream }",
            fragment.text,
        )
    }

    private fun injectedLanguageIn(source: String) =
        myFixture.configureByText("sample.xml", source).let {
            val host = hostIn() ?: return@let null
            InjectedLanguageManager.getInstance(project)
                .getInjectedPsiFiles(host)
                ?.firstOrNull()
                ?.first
                ?.language
        }

    private fun hostIn(): PsiLanguageInjectionHost? =
        PsiTreeUtil.findChildrenOfType(
            myFixture.file,
            PsiLanguageInjectionHost::class.java,
        ).firstOrNull { it.isValidHost }
}
