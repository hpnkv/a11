package a11

import java.nio.file.Files
import java.nio.file.Path
import kotlinx.coroutines.runBlocking
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * `Action.log`: what it writes, where it goes, and when it refuses.
 *
 * The lifecycle is the same as the C++ layer's and is pinned there; what is worth
 * pinning here is that the Kotlin port agrees about the port name, the metadata and
 * the refusals, so a Kotlin peer and a C++ peer read one contract.
 */
class ActionLogTest {
    private fun <T> ok(value: StatusOr<T>): T = when (value) {
        is Ok<T> -> value.value
        is Status -> throw AssertionError("expected ok, got $value")
    }

    private fun schema(name: String = "quiet") = ActionSchema(
        name = name,
        outputs = linkedMapOf("out" to ActionPortSchema(name = "out", type = "text/plain")),
    )

    /** Collects what the process sink is handed, and puts the sink back after. */
    private fun <T> capturing(body: (MutableList<LogRecord>) -> T): T {
        val records = mutableListOf<LogRecord>()
        setActionLogSink { records.add(it) }
        try {
            return body(records)
        } finally {
            setActionLogSink(null)
        }
    }

    private suspend fun runWith(action: Action, body: suspend (Action) -> Unit) {
        action.bindHandler { running -> body(running); Status.ok() }
        ok(action.run())
        action.wait()
    }

    @Test
    fun theLogPortCannotBeDeclaredInASchema() {
        val invalid = ActionSchema(
            name = "declared",
            outputs = linkedMapOf(
                ACTION_LOG_OUTPUT to ActionPortSchema(name = ACTION_LOG_OUTPUT, type = "text/plain"),
            ),
        )
        val status = invalid.validate()
        assertFalse(status.isOk)
        assertTrue(status.message.contains("is reserved"), status.message)
    }

    @Test
    fun theLogPortIsInNoSchemaAndNoActionMessage() = runBlocking {
        val action = ok(Action.create(schema(), ActionCreateOptions(id = "hidden")))
        assertFalse(action.getSchema().outputs.containsKey(ACTION_LOG_OUTPUT))
        assertEquals(listOf("out"), action.getActionMessage().outputs.map { it.name })
    }

    @Test
    fun aStringIsTextAndReachesTheSinkOnce() = runBlocking {
        capturing { records ->
            runBlocking {
                val action = ok(Action.create(schema(), ActionCreateOptions(id = "text")))
                runWith(action) { running -> running.log("a line", LogOptions(channel = "work")) }
                assertEquals(1, records.size)
                assertEquals(TEXT_MIMETYPE, records[0].mimetype)
                assertEquals("a line", String(records[0].data, Charsets.UTF_8))
                assertEquals(DEFAULT_LOG_LEVEL, records[0].level)
                assertEquals("work", records[0].channel)
                assertEquals("quiet", records[0].actionName)
                assertNotNull(records[0].timestampMillis)
            }
        }
    }

    @Test
    fun logfFillsPercentSAndLogfWithCarriesTheOptions() = runBlocking {
        capturing { records ->
            runBlocking {
                val action = ok(Action.create(schema(), ActionCreateOptions(id = "formatted")))
                runWith(action) { running ->
                    running.logf("read %s of %s pages", 3, 12)
                    running.logfWith(LogOptions(level = "warning"), "retrying %s", "a-url")
                    running.logf("100%% done")
                }
                assertEquals(
                    listOf("read 3 of 12 pages", "retrying a-url", "100% done"),
                    records.map { String(it.data, Charsets.UTF_8) },
                )
                assertEquals("warning", records[1].level)
            }
        }
    }

    @Test
    fun onlyARunningActionMayLogAndOnlyAtAKnownLevel() = runBlocking {
        val early = ok(Action.create(schema(), ActionCreateOptions(id = "early")))
        assertFalse(early.log("too soon").isOk)

        capturing { records ->
            runBlocking {
                val action = ok(Action.create(schema(), ActionCreateOptions(id = "bad-level")))
                var refused: Status? = null
                runWith(action) { running ->
                    refused = running.log("noisy", LogOptions(level = "verbose"))
                }
                assertNotNull(refused)
                assertFalse(refused!!.isOk)
                assertEquals(0, records.size)
            }
        }
    }

    @Test
    fun aClaimedLogPortCarriesTheChunksAndClosesItself() = runBlocking {
        capturing { records ->
            runBlocking {
                val action = ok(Action.create(schema(), ActionCreateOptions(id = "claimed")))
                val logs = ok(action.getLogNode())

                runWith(action) { running ->
                    running.log("first", LogOptions(channel = "work"))
                    running.log("second", LogOptions(level = "warning"))
                }

                // A claimed port owns presentation, so the sink is not also told.
                assertEquals(0, records.size)

                val seen = mutableListOf<Chunk>()
                while (true) {
                    val chunk = ok(logs.nextChunk()) ?: break
                    if (isStatusChunk(chunk)) continue
                    seen.add(chunk)
                }
                assertEquals(
                    listOf("first", "second"),
                    seen.map { String(it.data, Charsets.UTF_8) },
                )
                val first = logRecordFromChunk(seen[0])
                assertEquals("work", first.channel)
                assertEquals(DEFAULT_LOG_LEVEL, first.level)
                assertNotNull(first.timestampMillis)
                assertEquals("warning", logRecordFromChunk(seen[1]).level)
            }
        }
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

    @Test
    @Suppress("UNCHECKED_CAST")
    fun theReservedPortAndItsMetadataMatchTheFixture() {
        // The log port and its attribute names are a cross-language contract: a
        // peer in another language reads these chunks, so the words have to be the
        // same words. Pinned beside the status chunk for the same reason.
        val fixture = ok(A11Json.parse(testdata("log_chunk.json"))) as Map<*, *>
        assertEquals(ACTION_LOG_OUTPUT, fixture["port"])

        val attributes = fixture["attributes"] as Map<*, *>
        assertEquals(LOG_LEVEL_ATTRIBUTE, attributes["level"])
        assertEquals(LOG_INTERNAL_ATTRIBUTE, attributes["internal"])
        assertEquals(LOG_CHANNEL_ATTRIBUTE, attributes["channel"])
        assertEquals(LOG_FILE_ATTRIBUTE, attributes["file"])
        assertEquals(LOG_LINENO_ATTRIBUTE, attributes["lineno"])
        assertEquals(LOG_INTERNAL_TRUE, fixture["internal_true"])
        assertEquals(LOG_INTERNAL_FALSE, fixture["internal_false"])
        assertEquals(LOG_LEVELS, fixture["levels"])
        assertEquals(DEFAULT_LOG_LEVEL, fixture["default_level"])
        for ((written, meant) in fixture["level_aliases"] as Map<*, *>) {
            assertEquals(meant, parseLogLevel(written as String))
        }
    }

    @Test
    fun theLevelNamesAreTheFiveEveryLanguageAgreesOn() {
        assertEquals(listOf("debug", "info", "warning", "error", "critical"), LOG_LEVELS)
        assertEquals("warning", parseLogLevel("warn"))
        assertEquals("critical", parseLogLevel("FATAL"))
        assertEquals(DEFAULT_LOG_LEVEL, parseLogLevel(""))
        assertNull(parseLogLevel("chatty"))
    }
}
