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

import com.intellij.codeInsight.CodeInsightSettings
import com.intellij.lang.injection.InjectedLanguageManager
import com.intellij.openapi.command.WriteCommandAction
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.psi.PsiDocumentManager
import com.intellij.psi.PsiFile
import com.intellij.psi.PsiLanguageInjectionHost
import com.intellij.psi.util.PsiTreeUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import com.intellij.testFramework.fixtures.TempDirTestFixture
import com.intellij.testFramework.fixtures.impl.TempDirTestFixtureImpl
import org.junit.Assume.assumeTrue

class FlowSourceContextTest : BasePlatformTestCase() {
    override fun createTempDirTestFixture(): TempDirTestFixture = TempDirTestFixtureImpl()

    private val source = """
        declare flow test {}
        flow main {
          import "./nested.flow" as nested
          (run nested.work()).result -> _
        }
    """.trimIndent()
    private val library = """
        flow work exported {
          describe "A sibling module."
          out result: string
          "ready" -> result
        }
    """.trimIndent()

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

    private fun errors(file: PsiFile): List<FlowProblem> {
        val annotator = FlowAnnotator()
        return annotator.doAnnotate(annotator.collectInformation(file))
            .filter { it.severity == "error" }
    }

    private fun replace(file: PsiFile, text: String) {
        val document = PsiDocumentManager.getInstance(project).getDocument(file)!!
        WriteCommandAction.runWriteCommandAction(project) { document.setText(text) }
        PsiDocumentManager.getInstance(project).commitAllDocuments()
    }

    fun testDirectImportsRespectLocalDeclarationsAndCompleteSymbols() {
        val dependency = myFixture.addFileToProject("nested.flow", library)
        val root = myFixture.addFileToProject("root.flow", """
            flow main {
              declare flow work { out result: string }
              struct Report { text: string }
              from "./nested.flow" import work
              (run work()).result -> _
            }
        """.trimIndent())
        FileDocumentManager.getInstance().saveAllDocuments()
        assertTrue(errors(root).toString(), errors(root).isEmpty())
        val engine = FlowEngine.instance()
        val at = root.text.indexOf("run work") + "run w".length
        val completion = engine.complete(root.text, at, FlowSourceContext.fromFile(root))
        assertTrue(completion.toString(), (completion?.get("proposals") as? List<*>)?.any {
            (it as Map<*, *>)["name"] == "work"
        } == true)
        replace(dependency, "flow work exported { out result: integer }")
        val problems = errors(root)
        assertTrue(problems.toString(), problems.any {
            it.code == "flow.form.declaration-schema-mismatch" || it.code == "flow.form.declaration-conflict"
        })
        replace(root, "flow main {\n from \"./nested.flow\" import ")
        val choices = engine.complete(root.text, root.textLength, FlowSourceContext.fromFile(root))
        assertTrue(choices.toString(), (choices?.get("proposals") as? List<*>)?.any {
            (it as Map<*, *>)["name"] == "work"
        } == true)
    }

    fun testRelativeImportUsesTheCurrentFileFolder() {
        myFixture.addFileToProject("modules/nested.flow", library)
        val root = myFixture.addFileToProject("modules/root.flow", source)
        FileDocumentManager.getInstance().saveAllDocuments()
        assertEquals(root.virtualFile.canonicalPath, FlowSourceContext.fromFile(root).path)
        assertTrue(errors(root).toString(), errors(root).isEmpty())
    }

    fun testIncompleteNamespaceCompletionRespectsExports() {
        val dependency = myFixture.addFileToProject("nested.flow", "flow test {}")
        val root = myFixture.addFileToProject(
            "root.flow",
            "declare flow test { out output: string stream }\nflow main {\n" +
                " import \"./nested.flow\" as nested\n t = run nested.\n}",
        )
        FileDocumentManager.getInstance().saveAllDocuments()
        val at = root.text.indexOf("nested.\n") + "nested.".length
        val oldSetting = CodeInsightSettings.getInstance().AUTOCOMPLETE_ON_CODE_COMPLETION
        try {
            CodeInsightSettings.getInstance().AUTOCOMPLETE_ON_CODE_COMPLETION = false
            for (exported in listOf(false, true)) {
                replace(dependency, "flow test${if (exported) " exported" else ""} { out output: integer stream }")
                myFixture.configureFromExistingVirtualFile(root.virtualFile)
                myFixture.editor.caretModel.moveToOffset(at)
                val offered = myFixture.completeBasic()?.map { it.lookupString }.orEmpty()
                assertEquals(offered.toString(), exported, "test" in offered)
            }
        } finally {
            CodeInsightSettings.getInstance().AUTOCOMPLETE_ON_CODE_COMPLETION = oldSetting
        }
        replace(dependency, "flow test {}")
        replace(root, root.text.replace("run nested.", "run nested.test()"))
        val problem = errors(root).single { it.code == "flow.name.private-member" }
        assertTrue(problem.message, problem.message.contains("exists"))
        assertTrue(problem.message, problem.message.contains("private"))
        assertTrue(problem.message, problem.message.contains("exported"))
        assertFalse(errors(root).any { it.code == "flow.name.unknown" })
    }

    fun testIdenticalSourceInAnotherFolderHasItsOwnImportBase() {
        myFixture.addFileToProject("one/nested.flow", library)
        val one = myFixture.addFileToProject("one/root.flow", source)
        val two = myFixture.addFileToProject("two/root.flow", source)
        FileDocumentManager.getInstance().saveAllDocuments()
        assertTrue(errors(one).isEmpty())
        assertTrue(errors(two).any { it.code == "flow.name.module-not-found" })
        assertTrue(errors(one).isEmpty())
    }

    fun testSavedDependencyChangesInvalidateAnUnchangedRootCheck() {
        val dependency = myFixture.addFileToProject("nested.flow", library)
        val root = myFixture.addFileToProject("root.flow", source)
        FileDocumentManager.getInstance().saveAllDocuments()
        assertTrue(errors(root).isEmpty())
        replace(dependency, library.replace("result", "other"))
        FileDocumentManager.getInstance().saveAllDocuments()
        assertTrue(errors(root).any { it.code == "flow.name.unknown-port" })
    }

    fun testUnsavedDependenciesParticipateInAnalysisHoverCompletionAndNavigation() {
        val dependency = myFixture.addFileToProject("nested.flow", library.replace("result", "other"))
        val root = myFixture.addFileToProject("root.flow", source)
        FileDocumentManager.getInstance().saveAllDocuments()
        assertTrue(errors(root).any { it.code == "flow.name.unknown-port" })
        replace(dependency, library)
        val context = FlowSourceContext.fromFile(root)
        assertEquals(library, context.modules[dependency.virtualFile.canonicalPath])
        assertTrue(errors(root).isEmpty())
        val at = source.indexOf("nested.work") + "nested.".length
        val engine = FlowEngine.instance()
        val hover = engine.describe(source, at, context)
        assertTrue(hover.toString(), hover?.get("markdown").toString().contains("A sibling module."))
        val completion = engine.complete(source, at, context)
        val proposals = completion?.get("proposals") as List<*>
        assertTrue(proposals.toString(), proposals.any {
            (it as Map<*, *>)["name"] == "work"
        })
        myFixture.configureFromExistingVirtualFile(root.virtualFile)
        val target = FlowDeclarationHandler().getGotoDeclarationTargets(
            myFixture.file.findElementAt(at), at, myFixture.editor,
        )
        assertNotNull("no target for the imported flow", target)
        assertEquals(dependency.virtualFile, target!!.single().containingFile.virtualFile)
    }

    fun testExplicitContextTakesPrecedenceOverTheFileFolder() {
        myFixture.addFileToProject("supplied/nested.flow", library)
        val supplied = myFixture.addFileToProject("supplied/root.flow", "flow unused {}")
        val root = myFixture.addFileToProject("local/root.flow", source)
        FileDocumentManager.getInstance().saveAllDocuments()
        assertTrue(errors(root).any { it.code == "flow.name.module-not-found" })
        val context = FlowSourceContext(supplied.virtualFile.canonicalPath)
        root.putUserData(FlowSourceContext.KEY, context)
        assertSame(context, FlowSourceContext.fromFile(root))
        assertTrue(errors(root).isEmpty())
    }

    fun testInjectedFlowUsesItsHostFileFolder() {
        myFixture.addFileToProject("inline/nested.flow", library)
        val hostFile = myFixture.addFileToProject(
            "inline/host.xml",
            """<root note='flow main { import "./nested.flow" as nested }'/>""",
        )
        FileDocumentManager.getInstance().saveAllDocuments()
        val host = PsiTreeUtil.findChildOfType(hostFile, PsiLanguageInjectionHost::class.java)!!
        val injected = InjectedLanguageManager.getInstance(project)
            .getInjectedPsiFiles(host)!!.first().first as PsiFile
        assertEquals(hostFile.virtualFile.canonicalPath, FlowSourceContext.fromFile(injected).path)
        assertTrue(errors(injected).toString(), errors(injected).isEmpty())
    }
}
