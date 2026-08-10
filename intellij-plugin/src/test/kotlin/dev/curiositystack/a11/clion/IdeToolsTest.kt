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
            assertEquals("user_log_for_run", log["name"])
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
        val log = renamed["user_log_for_run"] as String
        // First line stands alone: it is all the folded box shows.
        assertEquals("Renamed bean → widget", log.lineSequence().first())
        assertTrue(log, log.contains("beans.xml") && log.contains("line 2"))

        val symbols = tools.runByName("get_file_symbols", emptyMap())
        val symbolLog = symbols["user_log_for_run"] as String
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

        fun namesWith(filters: Map<String, Any?>): List<String> {
            @Suppress("UNCHECKED_CAST")
            val symbols = tools.runByName("get_file_symbols", filters)["symbols"] as List<Map<String, Any?>>
            return symbols.map { it["name"] as String }
        }

        // No filters at all: the port is optional, so an empty call is valid.
        val everything = namesWith(emptyMap())
        assertTrue("$everything", everything.containsAll(listOf("root", "bean", "widget", "beanFactory")))

        // A name pattern matches anywhere in the name, case-insensitively.
        assertEquals(listOf("bean", "beanFactory"), namesWith(mapOf("filters" to mapOf("name_pattern" to "^BEAN"))))
        assertEquals(listOf("beanFactory"), namesWith(mapOf("filters" to mapOf("name_pattern" to "factory"))))

        // A line window, given as a 0-based offset and a count of lines.
        assertEquals(
            listOf("widget"),
            namesWith(mapOf("filters" to mapOf("line_offset" to 2, "line_limit" to 1))),
        )

        // Kinds match the parser's own names by substring, so "attribute" is enough.
        val attributes = namesWith(mapOf("filters" to mapOf("kinds" to listOf("attribute"))))
        assertEquals("$attributes", listOf("id"), attributes)
        assertTrue(namesWith(mapOf("filters" to mapOf("kinds" to listOf("nothing_like_this")))).isEmpty())

        // Filters compose.
        assertEquals(
            listOf("beanFactory"),
            namesWith(mapOf("filters" to mapOf("name_pattern" to "bean", "line_offset" to 3))),
        )

        assertRejected { tools.runByName("get_file_symbols", mapOf("filters" to mapOf("name_pattern" to "[unclosed"))) }
        assertRejected { tools.runByName("get_file_symbols", mapOf("filters" to mapOf("line_offset" to -1))) }
        assertRejected { tools.runByName("get_file_symbols", mapOf("filters" to mapOf("kinds" to listOf("")))) }
    }

    fun testFileSymbolFiltersAreDeclaredAsAnOptionalInput() {
        val port = inputPorts("get_file_symbols").single()
        assertEquals("filters", port["name"])
        assertEquals(false, port["required"])
        assertEquals(true, port["unary"])
        val fields = propertiesOf(schemaOf(port))
        assertEquals(setOf("name_pattern", "line_offset", "line_limit", "kinds"), fields.keys)
        // Nothing is required inside: each field narrows one dimension on its own.
        assertEquals(emptyList<String>(), schemaOf(port)["required"])
        assertEquals("array", (fields["kinds"] as Map<*, *>)["type"])
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
