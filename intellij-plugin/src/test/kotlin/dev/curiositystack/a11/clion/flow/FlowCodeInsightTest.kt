package dev.curiositystack.a11.clion.flow

import com.intellij.codeInsight.CodeInsightSettings
import com.intellij.codeInsight.documentation.DocumentationManager
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import org.junit.Assume.assumeTrue

/**
 * What hovering and completing actually show, through the platform.
 *
 * [FlowEngineTest] pins the protocol and the C++ tests pin the answers; this pins
 * the last mile, which is where both of these were lost: a hover over a name the
 * file declares was answered about the `flow` keyword it navigates to, and a
 * completed action had a description the popup never asked for.
 */
class FlowCodeInsightTest : BasePlatformTestCase() {

    override fun setUp() {
        super.setUp()
        assumeTrue("no a11-flow on this machine", FlowEngine.instance().available)
    }

    override fun tearDown() {
        try {
            CodeInsightSettings.getInstance().AUTO_POPUP_JAVADOC_INFO =
                CodeInsightSettings().AUTO_POPUP_JAVADOC_INFO
        } finally {
            super.tearDown()
        }
    }

    private val source = """
        struct Source {
          url: string required "Where it came from."
        }

        flow research {
          describe "Find out."
          in question: string required
          out answer: string

          hits = call interact_with_llm(prompt: question)
          hits.text | truncate 20 -> answer
        }

        flow outer {
          in q: string
          out a: string
          r = call research(question: q)
          r.answer -> a
        }
    """.trimIndent() + "\n"

    /** The documentation the platform would show with the caret at `at`. */
    private fun docAt(at: Int): String? {
        val file = myFixture.configureByText("t.flow", source)
        myFixture.editor.caretModel.moveToOffset(at)
        val context = file.findElementAt(at)
        val target = DocumentationManager.getInstance(project)
            .findTargetElement(myFixture.editor, at, file, context) ?: context
        return FlowDocumentationProvider().generateDoc(target!!, context)
    }

    fun `test the fixture really has a flow file and an engine`() {
        val file = myFixture.configureByText("t.flow", source)
        assertTrue("not a flow file: ${file.javaClass}", file is FlowPsiFile)
        assertTrue("engine unavailable", FlowEngine.instance().available)
        val about = FlowEngine.instance().describe(source, source.indexOf("struct Source") + 7)
        assertNotNull("describe returned null", about)
        assertEquals(about.toString(), true, about!!["found"])
    }

    fun `test hovering a struct this file declares describes the struct`() {
        val html = docAt(source.indexOf("struct Source") + "struct ".length)
        assertNotNull(html)
        assertTrue(html!!, html.contains("a struct of"))
        assertTrue(html, html.contains("Where it came from."))
    }

    fun `test hovering a flow this file declares describes the flow`() {
        val html = docAt(source.indexOf("call research(") + "call ".length)
        assertNotNull(html)
        assertTrue(html!!, html.contains("a flow of this file"))
    }

    fun `test hovering a stage shows the language's reference for it`() {
        val html = docAt(source.indexOf("| truncate") + 2)
        assertNotNull(html)
        assertTrue(html!!, html.contains("a pipeline stage"))
        assertTrue(html, html.contains("Takes:"))
        assertTrue(html, html.contains("Example:"))
        // The popup renders `code` and **bold**, and nothing is left as a
        // literal backtick or asterisk.
        assertFalse(html, html.contains("**"))
        assertFalse(html, html.contains("`"))
        // No `--` reaches a reader.
        assertFalse(html, html.contains("--"))
    }

    fun `test completing a stage offers it with the same reference`() {
        myFixture.configureByText("t.flow", source.replace("| truncate 20", "| "))
        myFixture.editor.caretModel.moveToOffset(
            myFixture.editor.document.text.indexOf("| ") + 2,
        )
        val elements = myFixture.completeBasic()
        assertNotNull("nothing was offered after a pipe", elements)
        val offered = elements!!.first { it.lookupString == "collect" }
        val provider = FlowDocumentationProvider()
        val element = provider.getDocumentationElementForLookupItem(
            psiManager,
            offered.getObject(),
            myFixture.file.findElementAt(myFixture.caretOffset - 1),
        )
        assertNotNull("no documentation element for a completed stage", element)
        val html = provider.generateDoc(element!!, null)
        assertTrue(html.orEmpty(), html.orEmpty().contains("a pipeline stage"))
    }

    fun `test completing an action offers it with its documentation`() {
        myFixture.configureByText(
            "t.flow",
            source.replace("call interact_with_llm(prompt: question)", "call i"),
        )
        myFixture.editor.caretModel.moveToOffset(
            myFixture.editor.document.text.indexOf("call i") + "call i".length,
        )
        val elements = myFixture.completeBasic()
        assertNotNull("nothing was offered at all", elements)
        val names = elements!!.map { it.lookupString }
        assertTrue(names.toString(), names.contains("interact_with_llm"))

        val offered = elements.first { it.lookupString == "interact_with_llm" }
        val provider = FlowDocumentationProvider()
        val element = provider.getDocumentationElementForLookupItem(
            psiManager,
            offered.getObject(),
            myFixture.file.findElementAt(myFixture.caretOffset - 1),
        )
        assertNotNull("no documentation element for a completed action", element)
        val html = provider.generateDoc(element!!, null)
        assertTrue(html.orEmpty(), html.orEmpty().contains("Route an LLM interaction"))
    }
}
