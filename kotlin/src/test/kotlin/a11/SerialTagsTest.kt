package a11

import a11.sdk.Interaction
import a11.sdk.ensureInteractionCodec
import a11.sdk.makeTextMessageInteraction
import java.nio.file.Files
import java.nio.file.Path
import java.util.Base64
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * The Kotlin half of the cross-language tag contract.
 *
 * `testdata/serial_tags.json` is the one table every language answers to, and
 * `testdata/interaction_golden.json` is
 * one interaction as the Python SDK writes
 * it. Between them these tests pin what a Kotlin client depends on when it
 * holds a conversation and hands it back to a Python backend turn after turn:
 * the tool calls and results inside each interaction must arrive as the objects
 * that were sent, and go back out as the payload that was received.
 */
class SerialTagsTest {
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

    private fun fixtureTags(): List<String> {
        val parsed = ok(A11Json.parse(testdata("serial_tags.json"))) as Map<*, *>
        return parsed.entries
            // `media_types` is the other half of the fixture and holds media
            // types, not tags; see `media types match the fixture`.
            .filter { !it.key.toString().startsWith("_") && it.key != "media_types" }
            .flatMap { (it.value as Map<*, *>).values.map { tag -> tag.toString() } }
    }

    private fun fixtureMediaTypes(): Map<*, *> {
        val parsed = ok(A11Json.parse(testdata("serial_tags.json"))) as Map<*, *>
        return parsed["media_types"] as Map<*, *>
    }

    private fun goldenChunk(): Pair<Chunk, ByteArray> {
        val golden = ok(A11Json.parse(testdata("interaction_golden.json"))) as Map<*, *>
        val data = Base64.getDecoder().decode(golden["base64"].toString())
        return Chunk(
            metadata = ChunkMetadata(mimetype = golden["mimetype"].toString()),
            data = data,
        ) to data
    }

    private fun kotlinTags(): List<String> = listOf(
        SerialTags.CHUNK_METADATA, SerialTags.CHUNK, SerialTags.NODE_REF,
        SerialTags.NODE_FRAGMENT, SerialTags.PORT, SerialTags.ACTION_MESSAGE,
        SerialTags.WIRE_MESSAGE, SerialTags.STATUS, SerialTags.TIME, SerialTags.DURATION,
        SerialTags.INTERACTION, SerialTags.PEER, SerialTags.ACTION_CONFIG,
        SerialTags.USAGE_METADATA, SerialTags.INTERACT_WITH_CLAUDE_CONFIG,
        SerialTags.INTERACT_WITH_CLAUDE_CODE_CONFIG,
        SerialTags.INTERACT_WITH_GEMINI_CONFIG, SerialTags.INTERACT_WITH_OLLAMA_CONFIG,
        SerialTags.INTERACT_WITH_GPT_CONFIG, SerialTags.INTERACT_WITH_CODEX_CONFIG,
        SerialTags.INTERACT_WITH_VLLM_CONFIG,
        SerialTags.INTERACT_WITH_GEMMA_CONFIG, SerialTags.AUDIO_BUFFER,
        SerialTags.AUDIO_INPUT_OPTIONS, SerialTags.SPEECH_RECOGNIZER_OPTIONS,
        SerialTags.AUDIO_DEVICE_INFO, SerialTags.AUDIO_CONTROL_EVENT,
        SerialTags.AUDIO_CAPTURE_EVENT, SerialTags.TRANSCRIPTION_EVENT,
    )

    @Test
    fun kotlinConstantsMatchTheSharedTable() {
        assertEquals(fixtureTags().sorted(), kotlinTags().sorted())
    }

    @Test
    fun aStatusChunkIsTheOneShapeEveryLanguageWrites() {
        val fixture = ok(A11Json.parse(testdata("status_chunk.json"))) as Map<*, *>
        assertEquals(fixture["mimetype"], ACTION_STATUS_MIMETYPE)
        assertEquals(fixture["close_attribute"], CLOSE_STATUS_ATTRIBUTE)

        for (entry in fixture["cases"] as List<*>) {
            val case = entry as Map<*, *>
            val name = case["name"].toString()
            @Suppress("UNCHECKED_CAST")
            val details = (case["details"] as List<*>).map { it as Map<String, Any?> }
            val code = StatusCode.fromValue((case["code"] as Number).toInt())
            assertNotNull(code, name)
            val status = Status(code, case["message"].toString(), details)

            val chunk = ok(statusToChunk(status))
            assertEquals(fixture["mimetype"], chunk.mimetype, name)
            assertEquals(case["base64"], Base64.getEncoder().encodeToString(chunk.data), name)
            assertTrue(!isCloseStatusChunk(chunk), name)

            val decoded = ok(decodeStatusChunk(chunk)).status
            assertEquals(status.code, decoded.code, name)
            assertEquals(status.message, decoded.message, name)
            assertEquals(details, decoded.details, name)
        }
    }

    @Test
    fun aClosureMarkerOnlyAddsTheSharedAttribute() {
        val fixture = ok(A11Json.parse(testdata("status_chunk.json"))) as Map<*, *>
        val plain = ok(statusToChunk(Status.ok()))
        val marker = ok(statusToChunk(Status.ok(), closing = true))

        // The marker rides on the metadata, so the payload is the plain status.
        assertContentEquals(plain.data, marker.data)
        assertEquals(plain.mimetype, marker.mimetype)
        assertTrue(isCloseStatusChunk(marker))
        assertTrue(!isCloseStatusChunk(plain))
        val attributes = assertNotNull(marker.metadata).attributes
        assertEquals(setOf(fixture["close_attribute"]), attributes.keys.toSet())
        assertContentEquals("1".toByteArray(), attributes[fixture["close_attribute"]])
    }

    @Test
    fun theRuntimeTypesAreRegisteredUnderTheirCanonicalTags() {
        ensureCoreWireValues()
        val registered = allWireValueCodecs().map { it.tag }.toSet()
        for (tag in listOf(
            SerialTags.CHUNK, SerialTags.CHUNK_METADATA, SerialTags.NODE_REF,
            SerialTags.NODE_FRAGMENT, SerialTags.PORT, SerialTags.ACTION_MESSAGE,
            SerialTags.WIRE_MESSAGE, SerialTags.STATUS,
        )) {
            assertTrue(registered.contains(tag), "no wire value codec for $tag")
        }
    }

    @Test
    fun pythonsInteractionDecodesIntoRealObjects() {
        ensureInteractionCodec()
        val registry = SerializationRegistry(registerDefaults = true)
        val (chunk, _) = goldenChunk()

        val interaction = ok(registry.fromChunk(chunk)) as Interaction

        assertEquals("golden-model", interaction["model"])
        // Turn metadata survives the language boundary.
        val content = interaction["content"] as List<*>
        assertTrue(content[0] is Chunk)
        assertEquals("application/json", (content[0] as Chunk).mimetype)
        val calls = interaction["action_calls"] as List<*>
        assertTrue(calls[0] is ActionMessage)
        assertEquals("rename_symbol", (calls[0] as ActionMessage).name)
        val inputs = (interaction["action_inputs"] as Map<*, *>)["p"] as List<*>
        assertTrue(inputs[0] is NodeFragment)
        assertEquals("n1", (inputs[0] as NodeFragment).id)
        assertTrue(interaction["status"] is Status)
        assertTrue(interaction["backend_specific_metadata"] is Map<*, *>)
    }

    @Test
    fun anInteractionHandedBackIsTheOneThatArrived() {
        ensureInteractionCodec()
        val registry = SerializationRegistry(registerDefaults = true)
        val (chunk, data) = goldenChunk()

        val interaction = ok(registry.fromChunk(chunk))
        val reencoded = ok(registry.toChunk(interaction))

        assertEquals("application/json;type=${SerialTags.INTERACTION}", reencoded.mimetype)
        assertContentEquals(data, reencoded.data)
    }

    @Test
    fun anInteractionBuiltHereIsTaggedForTheStrictPort() {
        ensureInteractionCodec()
        val registry = SerializationRegistry(registerDefaults = true)

        val chunk = ok(registry.toChunk(ok(makeTextMessageInteraction("hello"))))

        assertEquals("application/json;type=${SerialTags.INTERACTION}", chunk.mimetype)
    }

    @Test
    fun anInteractionBuiltHereIsTheOneThePythonSdkBuilds() {
        // This is what a chat client sends on the `interactions` port, and the
        // backend validates it against its own Interaction model -- so
        // `content` and `system_instructions` have to be Chunks, not the bare
        // values they hold.
        ensureInteractionCodec()
        val registry = SerializationRegistry(registerDefaults = true)
        val golden = ok(A11Json.parse(testdata("text_message_interaction_golden.json"))) as Map<*, *>
        val expected = ok(
            A11Json.parse(String(Base64.getDecoder().decode(golden["base64"].toString()))),
        ) as Map<*, *>

        val interaction = ok(
            makeTextMessageInteraction(
                golden["text"].toString(),
                golden["system_prompt"].toString(),
                registry = registry,
            ),
        )
        val chunk = ok(registry.toChunk(interaction))
        val actual = ok(A11Json.parse(String(chunk.data))) as Map<*, *>

        assertEquals(golden["mimetype"].toString(), chunk.mimetype)
        assertEquals(expected["content"], actual["content"])
        assertEquals(expected["system_instructions"], actual["system_instructions"])
    }

    @Test
    fun anOrdinaryMapIsStillOrdinaryData() {
        val registry = SerializationRegistry(registerDefaults = true)

        val chunk = ok(registry.toChunk(linkedMapOf<String, Any?>("code" to 0L, "message" to "hi")))

        // JSON already says this is an object; the mimetype does not repeat it.
        assertEquals("application/json", chunk.mimetype)
    }

    @Test
    fun aSerializedInteractionCarriesNoTags() {
        // The model's own shape says what everything is; nothing repeats it.
        // The chunk's `;type=` names the payload, and from there every nested
        // value sits in a field whose declared type identifies it.
        ensureInteractionCodec()
        val registry = SerializationRegistry(registerDefaults = true)
        fun keys(value: Any?): List<String> = when (value) {
            is Map<*, *> -> value.entries.flatMap { listOf(it.key.toString()) + keys(it.value) }
            is List<*> -> value.flatMap { keys(it) }
            else -> emptyList()
        }
        val interaction = ok(makeTextMessageInteraction("hi", "be brief"))

        val chunk = ok(registry.toChunk(interaction))

        val payload = ok(A11Json.parse(String(chunk.data)))
        assertEquals(emptyList(), keys(payload).filter { it.startsWith("!") })

        // And it still comes back as Chunks, from the shape alone.
        val decoded = ok(registry.fromChunk(chunk)) as Interaction
        assertTrue((decoded["content"] as List<*>)[0] is Chunk)
        val instructions = decoded["system_instructions"] as List<*>
        assertEquals("be brief", ok(registry.fromChunk(instructions[0] as Chunk)))
    }

    @Test
    fun noKeyInAPayloadIsEverReadAsAType() {
        // A payload is data. Nothing in it names a type, so nothing is escaped.
        // A11 once wrote a nested value's type as the sole key of a one-entry
        // object ({"!a11.Chunk": {...}}), which meant a caller's own map of
        // that shape had to be escaped on the way out. The type lives in the
        // chunk's metadata now, so these go out byte-for-byte as written.
        val registry = SerializationRegistry(registerDefaults = true)
        for (value in listOf(
            linkedMapOf<String, Any?>("!${SerialTags.CHUNK}" to "not one"),
            linkedMapOf<String, Any?>("!whatever" to listOf(1L, 2L)),
            linkedMapOf<String, Any?>("!a" to 1L, "b" to 2L),
        )) {
            assertEquals(value, ok(registry.fromChunk(ok(registry.toChunk(value)))))
        }
        val plain = linkedMapOf<String, Any?>("!a" to 1L, "b" to 2L)
        assertEquals("""{"!a":1,"b":2}""", ok(utf8Decode(ok(registry.toChunk(plain)).data)))
    }

    @Test
    fun anUnloadableTypeTagStillYieldsThePayload() {
        // A peer that never loaded the naming module still holds valid JSON,
        // and is entitled to read it as such rather than be refused.
        val registry = SerializationRegistry(registerDefaults = true)
        val chunk = Chunk(
            metadata = ChunkMetadata(mimetype = "application/json;type=some.other.Model"),
            data = utf8Encode("""{"anything":1}"""),
        )

        assertEquals(
            linkedMapOf<String, Any?>("anything" to 1L),
            ok(registry.fromChunk(chunk)),
        )
    }

    @Test
    fun mediaTypesMatchTheFixture() {
        // Pinned across languages exactly as the tags are. `text` and `bytes`
        // are the ones that matter: they are the defaults for a String and a
        // ByteArray, and a chunk using either carries no `type` parameter, so
        // the media type alone is what a peer has to go on.
        val media = fixtureMediaTypes()

        assertEquals(media["json"], JSON_MIMETYPE)
        assertEquals(media["msgpack"], MSGPACK_MIMETYPE)
        assertEquals(media["text"], TEXT_MIMETYPE)
        // This side's constant keeps its established name; what is pinned is
        // the fixture key, not the identifier each language spells it with.
        assertEquals(media["bytes"], OCTET_STREAM_MIMETYPE)
    }

    @Test
    fun aStringAndBytesTravelAsThemselves() {
        val registry = SerializationRegistry(registerDefaults = true)
        val media = fixtureMediaTypes()

        val text = ok(registry.toChunk("value"))
        val bytes = ok(registry.toChunk(byteArrayOf(0, -1)))

        assertEquals(media["text"], text.mimetype)
        assertEquals(media["bytes"], bytes.mimetype)
        // No framing: the bytes on the wire are the value.
        assertEquals("value", ok(utf8Decode(text.data)))
        assertContentEquals(byteArrayOf(0, -1), bytes.data)
        assertEquals("value", ok(registry.fromChunk(text)))
        assertContentEquals(byteArrayOf(0, -1), ok(registry.fromChunk(bytes)) as ByteArray)
    }
}
