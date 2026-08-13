package dev.curiositystack.a11.clion

import a11.sdk.getToolDefinitions
import a11.valueOrThrow
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.curiositystack.a11.clion.tools.IdeTools

/**
 * Verifies the IDE tools against a real (headless) platform fixture, exercising
 * the direct execution surface the JCEF bridge uses: `listDescriptors` (schemas)
 * and `runByName` (execution), plus the A11 registry that the kept Kotlin
 * session path builds from the same implementations.
 *
 * These tests deliberately do NOT drive a full A11 action (`action.run()`):
 * BasePlatformTestCase runs on the EDT, and a tool handler dispatched on A11's
 * background runtime that then hops back to the EDT (`invokeAndWait`) deadlocks
 * against the test-held EDT and hangs teardown's `checkEditorsReleased`. The
 * reverse-dispatch action path is covered by the `kotlin/` module's
 * `EndToEndTest`; here we test the IDE-facing surface directly, inline on the
 * EDT, where `invokeAndWait` runs synchronously and nothing is left pending.
 */
class IdeToolsTest : BasePlatformTestCase() {

    fun testRegistryExposesExpectedTools() {
        val (registry, descriptors) = IdeTools(project).buildRegistry()
        val names = registry.listRegisteredActions().toSet()
        assertTrue(
            names.containsAll(
                setOf("get_active_file", "get_open_editors", "get_selection", "find_file", "search_project"),
            ),
        )
        assertEquals(names.size, descriptors.size)
        assertEquals("get_active_file", descriptors.first { it["name"] == "get_active_file" }["name"])
    }

    fun testListDescriptorsMatchesRegistry() {
        val tools = IdeTools(project)
        val names = tools.listDescriptors().map { it["name"] as String }.toSet()
        assertEquals(tools.buildRegistry().first.listRegisteredActions().toSet(), names)
    }

    fun testArgumentTakingToolsDeclareASchemaTypedRequestPort() {
        assertTrue(inputPorts("get_open_editors").isEmpty())
        for ((name, field) in mapOf("find_file" to "name", "search_project" to "query")) {
            val port = inputPorts(name).single()
            assertEquals("request", port["name"])
            val schema = schemaOf(port)
            assertEquals("object", schema["type"])
            assertTrue(field in propertiesOf(schema))
            assertEquals(listOf(field), schema["required"])
            // A tool that cannot run without its arguments says so on the port.
            assertEquals(true, port["required"])
        }

        // `get_active_file` reads the whole file when asked for nothing, so every
        // field of its request — and the request itself — is optional.
        val slicePort = inputPorts("get_active_file").single()
        assertEquals(false, slicePort["required"])
        assertEquals(emptyList<String>(), schemaOf(slicePort)["required"])
        assertTrue("max_results" in propertiesOf(schemaOf(inputPorts("find_file").single())))
        // The slice bounds a model must respect are in the schema, not just in prose.
        val slice = propertiesOf(schemaOf(slicePort))
        assertEquals(0, (slice["line_offset"] as Map<*, *>)["minimum"])
        assertEquals(1, (slice["line_limit"] as Map<*, *>)["minimum"])
    }

    fun testEveryOutputPortIsTyped() {
        // Either the MIME type carries the value's type, or a JSON Schema does.
        for (name in IdeTools(project).listDescriptors().map { it["name"] as String }) {
            for (port in outputPorts(name)) {
                val described = port["type"] == "text/plain" || port["schema"] != null
                assertTrue("${name}.${port["name"]} should declare its type", described)
                assertTrue("${name}.${port["name"]} should be described", (port["description"] as String).isNotEmpty())
            }
        }
    }

    fun testEveryToolReportsAUserFacingRunLog() {
        for (name in IdeTools(project).listDescriptors().map { it["name"] as String }) {
            val log = outputPorts(name).filter { it["user_facing"] == true }.single()
            assertEquals("user_facing_log", log["name"])
            assertEquals("text/plain", log["type"])
            // One value or none, and never part of what the model is promised.
            assertEquals(true, log["unary"])
            assertEquals(false, log["required"])
        }
    }

    fun testRunLogsSummarizeWhatHappened() {
        myFixture.configureByText("beans.xml", "<root>\n  <bean/>\n</root>")
        val tools = IdeTools(project)

        val renamed = tools.runByName(
            "rename_symbol",
            mapOf("request" to mapOf("name" to "bean", "new_name" to "widget")),
        )
        val log = renamed["user_facing_log"] as String
        // First line stands alone: it is all the folded box shows.
        assertEquals("Renamed bean → widget", log.lineSequence().first())
        assertTrue(log, log.contains("beans.xml") && log.contains("line 2"))

        val symbols = tools.runByName("get_file_symbols", emptyMap())
        val symbolLog = symbols["user_facing_log"] as String
        assertTrue(symbolLog, symbolLog.lineSequence().first().startsWith("Found "))
        assertTrue(symbolLog, symbolLog.contains("- `root"))
    }

    fun testOutputsThatCanBeEmptyAreNotRequired() {
        // "required" would promise a value the tool cannot always produce: there
        // may be no selection, no open file, and no match.
        for (name in IdeTools(project).listDescriptors().map { it["name"] as String }) {
            for (port in outputPorts(name)) {
                assertEquals("${name}.${port["name"]} should be optional", false, port["required"])
            }
        }
        // Inputs the tool cannot run without stay required.
        assertEquals(true, inputPorts("find_file").single()["required"])
    }

    fun testTextOutputsCarryLinesAndPaths() {
        val activeFile = modelOutputs("get_active_file").associateBy { it["name"] as String }
        assertEquals(setOf("lines", "path"), activeFile.keys)
        assertEquals("text/plain", activeFile.getValue("lines")["type"])
        assertEquals(false, activeFile.getValue("lines")["unary"])
        assertEquals("text/plain", activeFile.getValue("path")["type"])
        assertEquals(true, activeFile.getValue("path")["unary"])

        for ((tool, output) in mapOf(
            "get_open_editors" to "files",
            "find_file" to "matches",
            "search_project" to "matches",
        )) {
            val port = modelOutputs(tool).single()
            assertEquals(output, port["name"])
            assertEquals("text/plain", port["type"])
            assertEquals(false, port["unary"])
        }
    }

    fun testSelectionSplitsMetadataFromLines() {
        val ports = modelOutputs("get_selection").associateBy { it["name"] as String }
        assertEquals(setOf("metadata", "lines"), ports.keys)

        val metadata = ports.getValue("metadata")
        assertEquals("application/json", metadata["type"])
        assertEquals(true, metadata["unary"])
        val fields = propertiesOf(schemaOf(metadata))
        assertEquals(setOf("path", "start_line", "end_line"), fields.keys)
        assertEquals(listOf("start_line", "end_line"), schemaOf(metadata)["required"])

        assertEquals("text/plain", ports.getValue("lines")["type"])
        assertEquals(false, ports.getValue("lines")["unary"])
    }

    // --- descriptor helpers --------------------------------------------------

    private fun descriptor(name: String): Map<String, Any?> =
        IdeTools(project).listDescriptors().first { it["name"] == name }

    @Suppress("UNCHECKED_CAST")
    private fun inputPorts(name: String) = descriptor(name)["inputs"] as List<Map<String, Any?>>

    @Suppress("UNCHECKED_CAST")
    private fun outputPorts(name: String) = descriptor(name)["outputs"] as List<Map<String, Any?>>

    /** The outputs a model is allowed to see: the run log is the user's alone. */
    private fun modelOutputs(name: String) = outputPorts(name).filter { it["user_facing"] != true }

    @Suppress("UNCHECKED_CAST")
    private fun schemaOf(port: Map<String, Any?>) = port["schema"] as Map<String, Any?>

    @Suppress("UNCHECKED_CAST")
    private fun propertiesOf(schema: Map<String, Any?>) = schema["properties"] as Map<String, Any?>

    fun testToolDefinitionsDescribeTheRequestFieldsToTheModel() {
        // The model-visible contract: `ToolAdapter` must use the port's declared
        // JSON Schema, not a bare `{"type": "object"}` derived from its MIME type.
        val (registry, _) = IdeTools(project).buildRegistry()
        val definition = getToolDefinitions(registry, listOf("find_file")).valueOrThrow().single()

        @Suppress("UNCHECKED_CAST")
        val request = ((definition["input_schema"] as Map<String, Any?>)["properties"] as Map<String, Any?>)
            .getValue("request") as Map<String, Any?>
        @Suppress("UNCHECKED_CAST")
        assertTrue("name" in (request["properties"] as Map<String, Any?>))
        assertEquals(listOf("name"), request["required"])
    }

    fun testMalformedRequestsAreRejected() {
        val tools = IdeTools(project)
        assertRejected { tools.runByName("find_file", emptyMap()) }
        assertRejected { tools.runByName("find_file", mapOf("request" to "not an object")) }
        assertRejected {
            tools.runByName("search_project", mapOf("request" to mapOf("query" to "widget", "max_results" to 0)))
        }
        assertRejected { tools.runByName("get_active_file", mapOf("request" to mapOf("line_offset" to -1))) }
        assertRejected {
            tools.runByName("get_active_file", mapOf("request" to mapOf("line_offset" to 0, "line_limit" to 0)))
        }
    }

    /** A request DTO that fails to parse must surface as an argument error. */
    private fun assertRejected(block: () -> Unit) {
        try {
            block()
            fail("Expected the malformed request to be rejected.")
        } catch (expected: IllegalArgumentException) {
            assertNotNull(expected.message)
        }
    }

    fun testActiveFileReturnsOnlyTheRequestedLines() {
        // configureByText opens the file in the fixture's editor, which the fixture
        // also releases on teardown; the handler hops to the EDT we already hold,
        // so invokeAndWait runs inline and nothing is left pending.
        myFixture.configureByText("slice.cpp", (1..5).joinToString("\n") { "line $it" })
        val tools = IdeTools(project)

        val page = tools.runByName("get_active_file", mapOf("request" to mapOf("line_offset" to 1, "line_limit" to 2)))
        assertEquals(listOf("line 2", "line 3"), page["lines"])
        assertTrue((page["path"] as String).endsWith("slice.cpp"))

        // Omitting the limit reads to the end of the file.
        val tail = tools.runByName("get_active_file", mapOf("request" to mapOf("line_offset" to 3)))
        assertEquals(listOf("line 4", "line 5"), tail["lines"])

        // An offset past the end is not an error, it just yields no lines.
        val past = tools.runByName("get_active_file", mapOf("request" to mapOf("line_offset" to 99)))
        assertEquals(emptyList<String>(), past["lines"])

        // No offset, or no request at all: the whole file, starting at the top.
        val all = tools.runByName("get_active_file", mapOf("request" to mapOf("line_limit" to 2)))
        assertEquals(listOf("line 1", "line 2"), all["lines"])
        val bare = tools.runByName("get_active_file", emptyMap())
        assertEquals((1..5).map { "line $it" }, bare["lines"])
    }

    fun testActiveFileCanNumberItsLinesLikeReadFileDoes() {
        myFixture.configureByText("numbered.cpp", (1..5).joinToString("\n") { "line $it" })
        val tools = IdeTools(project)

        val numbered = tools.runByName(
            "get_active_file",
            mapOf("request" to mapOf("include_line_numbers" to true)),
        )
        // 0-based, tab-separated: the same spelling `read_file` uses, so a numbered
        // line means one thing whichever tool produced it.
        assertEquals((1..5).map { "${it - 1}\tline $it" }, numbered["lines"])

        // The file's own numbers, not the slice's. A caller that paged in from line
        // 3 and got them renumbered from 0 would locate nothing with them.
        val page = tools.runByName(
            "get_active_file",
            mapOf("request" to mapOf("line_offset" to 3, "include_line_numbers" to true)),
        )
        assertEquals(listOf("3\tline 4", "4\tline 5"), page["lines"])

        // Off unless asked for: the numbers are not part of the file, and text that
        // will be quoted back into a patch has to come without them.
        val plain = tools.runByName("get_active_file", mapOf("request" to mapOf("line_offset" to 3)))
        assertEquals(listOf("line 4", "line 5"), plain["lines"])
    }

    fun testSelectionReturnsWhatTheUserHighlighted() {
        myFixture.configureByText("pick.txt", "alpha\n<selection>beta\ngamm</selection>a\ndelta")
        val tools = IdeTools(project)

        val selected = tools.runByName("get_selection", emptyMap())
        assertEquals(listOf("beta", "gamm"), selected["lines"])
        @Suppress("UNCHECKED_CAST")
        val metadata = selected["metadata"] as Map<String, Any?>
        assertEquals(2, metadata["start_line"])
        assertEquals(3, metadata["end_line"])
        assertTrue((metadata["path"] as String).endsWith("pick.txt"))

        // Nothing selected: nothing to describe, on either port.
        myFixture.editor.selectionModel.removeSelection()
        val empty = tools.runByName("get_selection", emptyMap())
        assertNull(empty["metadata"])
        assertEquals(emptyList<String>(), empty["lines"])
    }

    fun testFileSymbolsComeFromThePsiTree() {
        // XML is parsed by the platform itself, so this exercises real PSI without
        // depending on a language plugin: tags and attributes are PsiNamedElements.
        myFixture.configureByText("beans.xml", "<root>\n  <bean id=\"first\"/>\n</root>")

        val found = IdeTools(project).runByName("get_file_symbols", emptyMap())
        assertTrue((found["path"] as String).endsWith("beans.xml"))
        @Suppress("UNCHECKED_CAST")
        val symbols = found["symbols"] as List<Map<String, Any?>>
        val byName = symbols.associateBy { it["name"] as String }
        assertTrue("expected root and bean among $byName", byName.keys.containsAll(setOf("root", "bean")))

        val bean = byName.getValue("bean")
        assertEquals(2, bean["line"])
        assertEquals(4, bean["column"])
        assertTrue((bean["kind"] as String).isNotEmpty())
        // Source order, so the outer tag is reported before the one it contains.
        assertTrue(symbols.indexOfFirst { it["name"] == "root" } < symbols.indexOfFirst { it["name"] == "bean" })
    }

    fun testFileSymbolsHonoursItsFilters() {
        myFixture.configureByText(
            "beans.xml",
            "<root>\n  <bean id=\"first\"/>\n  <widget/>\n  <beanFactory/>\n</root>",
        )
        val tools = IdeTools(project)

        fun namesWith(request: Map<String, Any?>): List<String> {
            @Suppress("UNCHECKED_CAST")
            val symbols = tools.runByName("get_file_symbols", request)["symbols"] as List<Map<String, Any?>>
            return symbols.map { it["name"] as String }
        }

        // No filters at all: the port is optional, so an empty call is valid.
        val everything = namesWith(emptyMap())
        assertTrue("$everything", everything.containsAll(listOf("root", "bean", "widget", "beanFactory")))

        // A name pattern matches anywhere in the name, case-insensitively.
        assertEquals(listOf("bean", "beanFactory"), namesWith(mapOf("request" to mapOf("name_pattern" to "^BEAN"))))
        assertEquals(listOf("beanFactory"), namesWith(mapOf("request" to mapOf("name_pattern" to "factory"))))

        // A line window, given as a 0-based offset and a count of lines.
        assertEquals(
            listOf("widget"),
            namesWith(mapOf("request" to mapOf("line_offset" to 2, "line_limit" to 1))),
        )

        // Kinds match the parser's own names by substring, so "attribute" is enough.
        val attributes = namesWith(mapOf("request" to mapOf("kinds" to listOf("attribute"))))
        assertEquals("$attributes", listOf("id"), attributes)
        assertTrue(namesWith(mapOf("request" to mapOf("kinds" to listOf("nothing_like_this")))).isEmpty())

        // Filters compose.
        assertEquals(
            listOf("beanFactory"),
            namesWith(mapOf("request" to mapOf("name_pattern" to "bean", "line_offset" to 3))),
        )

        assertRejected { tools.runByName("get_file_symbols", mapOf("request" to mapOf("name_pattern" to "[unclosed"))) }
        assertRejected { tools.runByName("get_file_symbols", mapOf("request" to mapOf("line_offset" to -1))) }
        assertRejected { tools.runByName("get_file_symbols", mapOf("request" to mapOf("kinds" to listOf("")))) }
    }

    fun testFileSymbolFiltersAreDeclaredAsAnOptionalInput() {
        val port = inputPorts("get_file_symbols").single()
        assertEquals("request", port["name"])
        assertEquals(false, port["required"])
        assertEquals(true, port["unary"])
        val fields = propertiesOf(schemaOf(port))
        assertEquals(setOf("path", "name_pattern", "line_offset", "line_limit", "kinds"), fields.keys)
        // Nothing is required inside: each field narrows one dimension on its own.
        assertEquals(emptyList<String>(), schemaOf(port)["required"])
        assertEquals("array", (fields["kinds"] as Map<*, *>)["type"])
    }

    fun testFileSymbolsCanBeAskedAboutAFileThatIsNotOpen() {
        // Added, not opened: `path` is what makes a file with no editor readable.
        val other = myFixture.addFileToProject("other.xml", "<other>\n  <thing/>\n</other>")
        myFixture.configureByText("beans.xml", "<root>\n  <bean/>\n</root>")
        val tools = IdeTools(project)

        fun namesIn(request: Map<String, Any?>): List<String> {
            @Suppress("UNCHECKED_CAST")
            val symbols = tools.runByName("get_file_symbols", request)["symbols"] as List<Map<String, Any?>>
            return symbols.map { it["name"] as String }
        }

        // No path: the active editor, as before.
        assertTrue(namesIn(emptyMap()).contains("root"))
        // A path: that file, wherever it is and whether or not anyone opened it.
        val named = namesIn(mapOf("request" to mapOf("path" to other.virtualFile.path)))
        assertTrue("$named", named.containsAll(listOf("other", "thing")))
        assertFalse("$named", named.contains("root"))
        // Project-relative works too, and the reported path is the file's own.
        val relative = tools.runByName("get_file_symbols", mapOf("request" to mapOf("path" to "other.xml")))
        assertEquals(other.virtualFile.path, relative["path"])
        // The rest of the request still narrows, on that file.
        assertEquals(
            listOf("thing"),
            namesIn(mapOf("request" to mapOf("path" to "other.xml", "name_pattern" to "thing"))),
        )
        assertRejected { tools.runByName("get_file_symbols", mapOf("request" to mapOf("path" to "nowhere.xml"))) }
    }

    fun testReadFileReturnsTheRequestedLines() {
        val file = myFixture.addFileToProject("sliced.txt", (0..5).joinToString("\n") { "line $it" })
        val tools = IdeTools(project)
        val path = file.virtualFile.path

        fun readWith(request: Map<String, Any?>): Map<String, Any?> =
            tools.runByName("read_file", mapOf("request" to (mapOf("path" to path) + request)))

        // 0-based, end_line inclusive — the coordinates get_error_highlights uses.
        assertEquals(listOf("line 1", "line 2"), readWith(mapOf("start_line" to 1, "end_line" to 2))["lines"])
        // Omitting either end means "to the edge of the file".
        assertEquals((0..5).map { "line $it" }, readWith(emptyMap())["lines"])
        assertEquals(listOf("line 4", "line 5"), readWith(mapOf("start_line" to 4))["lines"])
        // A range past the end is not an error; it just yields nothing.
        assertEquals(emptyList<String>(), readWith(mapOf("start_line" to 99))["lines"])
        // An end past the end stops at the end.
        assertEquals(listOf("line 5"), readWith(mapOf("start_line" to 5, "end_line" to 99))["lines"])
        assertEquals(path, readWith(emptyMap())["path"])

        // Numbers when asked for, and they are the same 0-based ones.
        assertEquals(
            listOf("1\tline 1", "2\tline 2"),
            readWith(mapOf("start_line" to 1, "end_line" to 2, "include_line_numbers" to true))["lines"],
        )

        assertRejected { tools.runByName("read_file", emptyMap()) }
        assertRejected { tools.runByName("read_file", mapOf("request" to mapOf("start_line" to 1))) }
        assertRejected { readWith(mapOf("start_line" to 3, "end_line" to 1)) }
        assertRejected { readWith(mapOf("include_line_numbers" to "yes")) }
        assertRejected { tools.runByName("read_file", mapOf("request" to mapOf("path" to "nowhere.txt"))) }
    }

    fun testReadFileSeesWhatTheEditorHoldsRatherThanTheDisk() {
        myFixture.configureByText("live.txt", "first\nsecond")
        val path = myFixture.file.virtualFile.path
        // An edit nobody has saved: the tool reads the document, so it is there.
        com.intellij.openapi.command.WriteCommandAction.runWriteCommandAction(project) {
            myFixture.editor.document.setText("first\nedited")
        }
        val read = IdeTools(project).runByName("read_file", mapOf("request" to mapOf("path" to path)))
        assertEquals(listOf("first", "edited"), read["lines"])
    }

    fun testApplyPatchTakesTwoTextInputs() {
        val ports = inputPorts("apply_patch").associateBy { it["name"] as String }
        assertEquals(setOf("path", "patch"), ports.keys)
        for (port in ports.values) {
            assertEquals("text/plain", port["type"])
            assertEquals(true, port["unary"])
            // Neither is optional: there is nothing to do without both.
            assertEquals(true, port["required"])
        }
        val metadata = modelOutputs("apply_patch").single()
        assertEquals("metadata", metadata["name"])
        assertEquals(
            setOf("path", "hunks", "first_line", "added", "removed"),
            propertiesOf(schemaOf(metadata)).keys,
        )
    }

    fun testApplyPatchEditsTheFileAndCanBeUndone() {
        myFixture.configureByText("patched.txt", "alpha\nbeta\ngamma\n")
        val path = myFixture.file.virtualFile.path
        val document = myFixture.editor.document
        val tools = IdeTools(project)

        val done = tools.runByName(
            "apply_patch",
            mapOf(
                "path" to path,
                "patch" to "@@ -1,3 +1,3 @@\n alpha\n-beta\n+BETA\n gamma\n",
            ),
        )
        assertEquals("alpha\nBETA\ngamma\n", document.text)
        @Suppress("UNCHECKED_CAST")
        val metadata = done["metadata"] as Map<String, Any?>
        assertEquals(path, metadata["path"])
        assertEquals(1, metadata["hunks"])
        assertEquals(0, metadata["first_line"])
        assertEquals(3, metadata["added"])
        assertEquals(3, metadata["removed"])
        val log = done["user_facing_log"] as String
        assertTrue(log, log.lineSequence().first().startsWith("Patched patched.txt (1 hunk"))
        assertTrue(log, log.contains("```diff"))

        // One command, so one Undo takes the whole patch back — the same
        // reversibility a rename has.
        val undo = com.intellij.openapi.command.undo.UndoManager.getInstance(project)
        val editor = com.intellij.openapi.fileEditor.FileEditorManager.getInstance(project)
            .getSelectedEditor(myFixture.file.virtualFile)
        assertTrue("the patch should be undoable", undo.isUndoAvailable(editor))
        undo.undo(editor)
        assertEquals("alpha\nbeta\ngamma\n", document.text)
    }

    fun testApplyPatchFindsAHunkThatMovedAndRefusesOneThatDoesNotFit() {
        myFixture.configureByText("moved.txt", "one\ntwo\nthree\nfour\nfive\n")
        val path = myFixture.file.virtualFile.path
        val tools = IdeTools(project)

        // The header says line 1; the context is at line 3. Context wins.
        tools.runByName(
            "apply_patch",
            mapOf("path" to path, "patch" to "@@ -1,2 +1,2 @@\n-three\n+THREE\n four\n"),
        )
        assertEquals("one\ntwo\nTHREE\nfour\nfive\n", myFixture.editor.document.text)

        // Several hunks, in file order, applied as one edit.
        tools.runByName(
            "apply_patch",
            mapOf(
                "path" to path,
                "patch" to "@@ -1,1 +1,1 @@\n-one\n+ONE\n@@ -5,1 +5,1 @@\n-five\n+FIVE\n",
            ),
        )
        assertEquals("ONE\ntwo\nTHREE\nfour\nFIVE\n", myFixture.editor.document.text)

        // A hunk whose context is not in the file at all: refused, and nothing
        // else in the patch is applied either.
        val before = myFixture.editor.document.text
        val refused = try {
            tools.runByName(
                "apply_patch",
                mapOf(
                    "path" to path,
                    "patch" to "@@ -1,1 +1,1 @@\n-two\n+TWO\n@@ -9,1 +9,1 @@\n-nothing like this\n+x\n",
                ),
            )
            fail("expected the unmatched hunk to be refused")
            return
        } catch (expected: IllegalArgumentException) {
            expected.message ?: ""
        }
        assertTrue(refused, refused.contains("does not match the file"))
        assertTrue(refused, refused.contains("nothing like this"))
        assertEquals("nothing should have been applied", before, myFixture.editor.document.text)

        // Not a patch at all.
        assertRejected { tools.runByName("apply_patch", mapOf("path" to path, "patch" to "just some prose")) }
        assertRejected { tools.runByName("apply_patch", mapOf("path" to path)) }
    }

    fun testApplyPatchAddsAndRemovesLines() {
        myFixture.configureByText("grow.txt", "keep\nremove me\nkeep too\n")
        val path = myFixture.file.virtualFile.path
        val tools = IdeTools(project)

        // A hunk that only adds, placed by its header.
        tools.runByName(
            "apply_patch",
            mapOf("path" to path, "patch" to "@@ -1,0 +1,1 @@\n+added first\n"),
        )
        assertEquals("added first\nkeep\nremove me\nkeep too\n", myFixture.editor.document.text)

        // And one that only removes.
        tools.runByName(
            "apply_patch",
            mapOf("path" to path, "patch" to "@@ -3,1 +3,0 @@\n-remove me\n"),
        )
        assertEquals("added first\nkeep\nkeep too\n", myFixture.editor.document.text)

        // `---`/`+++` headers are allowed and ignored: the path is a separate input.
        tools.runByName(
            "apply_patch",
            mapOf(
                "path" to path,
                "patch" to "--- a/somewhere/else.txt\n+++ b/somewhere/else.txt\n@@ -2,1 +2,1 @@\n-keep\n+KEPT\n",
            ),
        )
        assertEquals("added first\nKEPT\nkeep too\n", myFixture.editor.document.text)
    }

    fun testApplyPatchTakesWhatAModelActuallyWrites() {
        // Verbatim from a local model asked to fix an undeclared variable: one
        // added line, one context line, and a header whose numbers do not agree
        // with each other or with the file. The context is what places it.
        myFixture.configureByText("model.cpp", "#include <memory>\n\nint main() {\n  int x = y;\n  return 0\n}\n")
        val tools = IdeTools(project)
        val path = myFixture.file.virtualFile.path

        tools.runByName(
            "apply_patch",
            mapOf("path" to path, "patch" to "@@ -4,1 +5,2 @@\n+  int y;\n  int x = y;"),
        )
        assertEquals(
            "#include <memory>\n\nint main() {\n  int y;\n  int x = y;\n  return 0\n}\n",
            myFixture.editor.document.text,
        )

        tools.runByName(
            "apply_patch",
            mapOf("path" to path, "patch" to "@@ -5,1 +5,1 @@\n-  return 0\n+  return 0;"),
        )
        assertEquals(
            "#include <memory>\n\nint main() {\n  int y;\n  int x = y;\n  return 0;\n}\n",
            myFixture.editor.document.text,
        )

        // Also from a model: the markers indented, and a header whose numbers are
        // punctuated wrongly. Both are read for what they are, because the file is
        // what settles which line is which.
        tools.runByName(
            "apply_patch",
            mapOf("path" to path, "patch" to "@@ -4,1,4,1 @@\n -  int y;\n +  int y = 0;"),
        )
        assertEquals(
            "#include <memory>\n\nint main() {\n  int y = 0;\n  int x = y;\n  return 0;\n}\n",
            myFixture.editor.document.text,
        )

        // But indentation that is simply wrong is refused: a patch claiming a tab
        // where the file has spaces is not this file's text.
        assertRejected {
            tools.runByName(
                "apply_patch",
                mapOf("path" to path, "patch" to "@@ -5,1 +5,2 @@\n+\tint z = 1;\n \tint x = y;"),
            )
        }
    }

    fun testErrorHighlightsDeclaresARangeRequestAndAStreamOfHighlights() {
        val port = inputPorts("get_error_highlights").single()
        assertEquals("request", port["name"])
        assertEquals(true, port["required"])
        val fields = propertiesOf(schemaOf(port))
        assertEquals(setOf("path", "start_line", "end_line"), fields.keys)
        // The file is the only thing a caller must give; the range defaults to all of it.
        assertEquals(listOf("path"), schemaOf(port)["required"])
        assertEquals(0, (fields["start_line"] as Map<*, *>)["minimum"])
        assertEquals(0, (fields["end_line"] as Map<*, *>)["minimum"])

        val outputs = modelOutputs("get_error_highlights").associateBy { it["name"] as String }
        assertEquals(setOf("highlights", "path"), outputs.keys)
        // One value per highlight, streamed as they are found.
        val highlights = outputs.getValue("highlights")
        assertEquals("application/json", highlights["type"])
        assertEquals(false, highlights["unary"])
        assertEquals(
            setOf(
                "severity", "start_line", "end_line", "start_column", "end_column", "text", "message", "tooltip",
            ),
            propertiesOf(schemaOf(highlights)).keys,
        )
        // Everything but the tooltip is always there: a highlight need not have one.
        assertEquals(
            listOf("severity", "start_line", "end_line", "start_column", "end_column", "text", "message"),
            schemaOf(highlights)["required"],
        )
    }

    fun testErrorHighlightsReportsWhatTheEditorUnderlines() {
        // An unclosed XML tag: the platform's own annotator flags it, so this needs
        // no language plugin. `doHighlighting` runs the daemon for the fixture's
        // file, which is what leaves the markup this tool reads for an open file —
        // the only path available on the EDT the fixture holds.
        val file = myFixture.configureByText("broken.xml", "<root>\n  <bean>\n</root>\n")
        myFixture.doHighlighting()
        val path = file.virtualFile.path

        val all = IdeTools(project).runByName("get_error_highlights", mapOf("request" to mapOf("path" to path)))
        assertEquals(path, all["path"])
        @Suppress("UNCHECKED_CAST")
        val highlights = all["highlights"] as List<Map<String, Any?>>
        assertFalse("expected the unclosed tag to be flagged", highlights.isEmpty())

        val first = highlights.first()
        assertEquals("ERROR", first["severity"])
        // Positions are 0-based, so they can be fed straight back as a range bound.
        assertTrue("$first", (first["start_line"] as Int) >= 0)
        assertTrue("$first", (first["end_column"] as Int) >= (first["start_column"] as Int))
        assertTrue("$first", (first["message"] as String).isNotEmpty())
        // The tooltip is plain text: the platform writes it as escaped HTML.
        val tooltip = first["tooltip"] as? String
        if (tooltip != null) assertFalse(tooltip, tooltip.contains("<html") || tooltip.contains("&lt;"))

        // Sorted by position, and every entry carries the text it underlines.
        val lines = highlights.map { it["start_line"] as Int }
        assertEquals(lines.sorted(), lines)
        for (highlight in highlights) assertNotNull(highlight["text"])

        val log = all["user_facing_log"] as String
        assertTrue(log, log.lineSequence().first().startsWith("Found "))
        assertTrue(log, log.contains("broken.xml"))
    }

    fun testErrorHighlightsNarrowsToTheRequestedLines() {
        val file = myFixture.configureByText("broken.xml", "<root>\n  <bean>\n</root>\n")
        myFixture.doHighlighting()
        val path = file.virtualFile.path
        val tools = IdeTools(project)

        fun highlightsIn(range: Map<String, Any?>): List<Map<String, Any?>> {
            val request = mapOf("request" to (mapOf("path" to path) + range))
            @Suppress("UNCHECKED_CAST")
            return tools.runByName("get_error_highlights", request)["highlights"] as List<Map<String, Any?>>
        }

        val everything = highlightsIn(emptyMap())
        assertFalse(everything.isEmpty())
        // An omitted start_line is the top of the file, an omitted end_line its end.
        assertEquals(everything.size, highlightsIn(mapOf("start_line" to 0)).size)
        // A range above every highlight reports nothing.
        assertEquals(emptyList<Any?>(), highlightsIn(mapOf("start_line" to 99)))

        // A single-line range reports the highlights overlapping that line, and only
        // those — which is every highlight whose own span covers it.
        val line = everything.first()["start_line"] as Int
        val onThatLine = highlightsIn(mapOf("start_line" to line, "end_line" to line))
        assertEquals(
            everything.filter { (it["start_line"] as Int) <= line && (it["end_line"] as Int) >= line },
            onThatLine,
        )
        assertTrue("$onThatLine", onThatLine.isNotEmpty())

        // Malformed requests are rejected rather than guessed at.
        assertRejected { tools.runByName("get_error_highlights", emptyMap()) }
        assertRejected { tools.runByName("get_error_highlights", mapOf("request" to mapOf("start_line" to 1))) }
        assertRejected {
            tools.runByName("get_error_highlights", mapOf("request" to mapOf("path" to path, "start_line" to -1)))
        }
        assertRejected {
            tools.runByName(
                "get_error_highlights",
                mapOf("request" to mapOf("path" to path, "start_line" to 3, "end_line" to 1)),
            )
        }
        assertRejected {
            tools.runByName("get_error_highlights", mapOf("request" to mapOf("path" to "/nowhere/at/all.xml")))
        }
    }

    fun testRenameSymbolRewritesTheFile() {
        myFixture.configureByText("beans.xml", "<root>\n  <bean/>\n</root>")

        val renamed = IdeTools(project)
            .runByName("rename_symbol", mapOf("request" to mapOf("name" to "bean", "new_name" to "widget")))
        @Suppress("UNCHECKED_CAST")
        val metadata = renamed["metadata"] as Map<String, Any?>
        assertEquals("bean", metadata["previous_name"])
        assertEquals("widget", metadata["new_name"])
        assertEquals(2, metadata["line"])
        assertNotNull(metadata["usages"])
        assertEquals("<root>\n  <widget/>\n</root>", myFixture.editor.document.text)
    }

    fun testRenameSymbolReportsAmbiguityInsteadOfGuessing() {
        myFixture.configureByText("beans.xml", "<root>\n  <bean/>\n  <bean/>\n</root>")
        val tools = IdeTools(project)

        val ambiguous = try {
            tools.runByName("rename_symbol", mapOf("request" to mapOf("name" to "bean", "new_name" to "widget")))
            fail("Expected the repeated name to be reported as ambiguous.")
            return
        } catch (expected: IllegalArgumentException) {
            expected.message ?: ""
        }
        // The message must carry the positions, so a caller can disambiguate.
        assertTrue(ambiguous, ambiguous.contains("2:4") && ambiguous.contains("3:4"))

        // Which the line does; the other occurrence is left alone.
        tools.runByName(
            "rename_symbol",
            mapOf("request" to mapOf("name" to "bean", "new_name" to "widget", "line" to 3, "column" to 5)),
        )
        assertEquals("<root>\n  <bean/>\n  <widget/>\n</root>", myFixture.editor.document.text)

        assertRejected { tools.runByName("rename_symbol", mapOf("request" to mapOf("name" to "nope", "new_name" to "x"))) }
    }

    fun testRunByNameDrivesTheIdeImplementations() {
        // The JCEF bridge path: run a tool directly by name with its inputs keyed
        // by port name.
        // Add the file WITHOUT opening an editor (index-only tool), so nothing
        // needs the EDT and teardown's editor-release check has nothing to wait
        // on. (Editor-backed tools like get_active_file are exercised manually
        // through the Action explorer.)
        myFixture.addFileToProject("widget.cpp", "// hello")

        val found = IdeTools(project).runByName("find_file", mapOf("request" to mapOf("name" to "widget.cpp")))
        assertTrue((found["matches"] as List<*>).any { (it as String).endsWith("widget.cpp") })
    }
}
