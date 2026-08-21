package a11

import java.nio.file.Files
import java.nio.file.Path
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * The Kotlin half of the schema-document contract.
 *
 * `testdata/actions/schema_document.json` is one `a11.actions/v1` document as
 * the native writer produces it. These tests pin that this side reads it into
 * the same schema and writes the same document back -- which is what replaces
 * four hand-copied handshake schemas, and the only reason to trust that a
 * gateway asking the IDE plugin what it serves gets an answer it understands.
 */
class SchemaDocumentTest {
    private fun <T> ok(value: StatusOr<T>): T = when (value) {
        is Ok<T> -> value.value
        is Status -> throw AssertionError("expected ok, got $value")
    }

    private fun testdata(name: String): String {
        // The Gradle build runs in kotlin/; the shared fixtures sit beside it.
        var directory: Path? = Path.of("").toAbsolutePath()
        while (directory != null && !Files.exists(directory.resolve("testdata/$name"))) {
            directory = directory.parent
        }
        assertNotNull(directory, "could not find testdata/$name above the working directory")
        return Files.readString(directory.resolve("testdata/$name"))
    }

    private fun fixture(view: String): List<Map<*, *>> {
        val parsed = ok(A11Json.parse(testdata("actions/schema_document.json"))) as Map<*, *>
        return ok(schemasInDocument(parsed[view]))
    }

    @Test
    fun `a native document reads into a schema and writes back unchanged`() {
        val entries = fixture("callable")
        assertEquals(1, entries.size)
        val entry = entries[0]

        val schema = ok(schemaFromJson(entry))
        assertEquals("shell_execute", schema.name)
        assertEquals("Run a shell command.", schema.description)
        assertEquals(listOf("command", "parameters"), schema.inputs.keys.sorted())
        assertTrue(schema.inputs["command"]!!.required)
        assertTrue(schema.inputs["command"]!!.unary)
        // Streaming, and it has to survive: the port structs in A11 disagree on
        // what an absent `unary` means, so a reader filling one in would invert
        // this.
        assertFalse(schema.outputs["output_lines"]!!.unary)
        assertTrue(schema.outputs["status"]!!.unary)
        assertEquals("$", schema.outputToJsonField["status"])
        // A port's value schema travels, which is what lets a model be shown a
        // remote tool's real argument types.
        assertEquals("string", schema.inputs["command"]!!.jsonSchema?.get("type"))

        // And back: the same entry.
        val written = schemaToJson(schema, entry["runnable"] == true)
        assertEquals(entry, written)
    }

    @Test
    fun `unary is always written, never omitted when false`() {
        for (entry in fixture("callable")) {
            for (key in listOf("inputs", "outputs")) {
                for (port in entry[key] as? List<*> ?: emptyList<Any?>()) {
                    val written = port as Map<*, *>
                    assertTrue(
                        written.containsKey("unary"),
                        "${entry["name"]}.${written["name"]} omitted unary",
                    )
                }
            }
        }
    }

    @Test
    fun `a registry writes the document the native writer wrote`() {
        val schema = ok(schemaFromJson(fixture("callable")[0]))
        val registry = ActionRegistry()
        // Schema only, no handler -- which is what `runnable: false` in the
        // fixture says, and how "this one lives on the peer" is spelled.
        assertTrue(registry.register(schema.name, schema).isOk)

        val own = registry.listRegisteredActions().filterNot { it.startsWith("__") }
        assertEquals(listOf("shell_execute"), own)
        assertEquals(
            fixture("callable")[0],
            schemaToJson(ok(registry.getSchema("shell_execute")), false),
        )
    }

    @Test
    fun `a builtin is answered by a registry that holds nothing`() {
        val registry = ActionRegistry()
        assertEquals(
            listOf("__get_schema__", "__list_actions__", "__ping"),
            builtinActionNames(),
        )
        for (name in builtinActionNames()) {
            assertTrue(registry.isRegistered(name), name)
            assertTrue(registry.getSchema(name) is Ok, name)
            assertTrue(registry.getHandler(name) is Ok, name)
        }
        // Refused rather than shadowed: a peer must always be answerable.
        val impostor = ok(
            schemaFromJson(
                mapOf(
                    "name" to LIST_ACTIONS_NAME,
                    "outputs" to listOf(
                        mapOf("name" to "actions", "type" to JSON_MIMETYPE, "unary" to true),
                    ),
                ),
            ),
        )
        assertFalse(registry.register(LIST_ACTIONS_NAME, impostor).isOk)
        assertFalse(registry.unregister(PING_NAME).isOk)
    }

    @Test
    fun `the format tag matches the native one`() {
        val parsed = ok(A11Json.parse(testdata("actions/schema_document.json"))) as Map<*, *>
        val document = parsed["callable"] as Map<*, *>
        assertEquals(SCHEMA_DOCUMENT_FORMAT, document["format"])
    }
}
