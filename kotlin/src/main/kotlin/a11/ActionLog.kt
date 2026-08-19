package a11

/**
 * Log levels, log-chunk metadata and the process-wide action log sink.
 *
 * The Kotlin twin of `cpp/a11/actions/log.h`. An [Action] narrates what it is
 * doing through [Action.log], which turns the value it is handed into a [Chunk]
 * on the reserved [ACTION_LOG_OUTPUT] port; this file holds the vocabulary
 * everything that writes or reads such a chunk shares.
 */

/** The five severities every language A11 binds to agrees on, quietest first. */
val LOG_LEVELS: List<String> = listOf("debug", "info", "warning", "error", "critical")

/** The level a log written without one carries. */
const val DEFAULT_LOG_LEVEL = "info"

/**
 * Chunk metadata attribute naming the log's level.
 *
 * The log attributes are unprefixed, unlike [CLOSE_STATUS_ATTRIBUTE], because
 * they only ever appear on the reserved log port: there is no user metadata there
 * to collide with.
 */
const val LOG_LEVEL_ATTRIBUTE = "level"
/** Attribute marking a log as A11's own bookkeeping: `"true"` or `"false"`. */
const val LOG_INTERNAL_ATTRIBUTE = "internal"
/** Attribute naming a free-form channel a consumer may filter on. */
const val LOG_CHANNEL_ATTRIBUTE = "channel"
/** Attribute naming the source file the log came from. */
const val LOG_FILE_ATTRIBUTE = "file"
/** Attribute naming the source line the log came from. */
const val LOG_LINENO_ATTRIBUTE = "lineno"
/** The value [LOG_INTERNAL_ATTRIBUTE] takes when the log is internal. */
const val LOG_INTERNAL_TRUE = "true"
/** The value it takes when the log is not. */
const val LOG_INTERNAL_FALSE = "false"

/** Whether [name] is one of [LOG_LEVELS], in either case. */
fun isLogLevel(name: String): Boolean = name.lowercase() in LOG_LEVELS

/**
 * The canonical level [name] means, or `null` when it is not one.
 *
 * Accepts `warn` for `warning` and `fatal` for `critical` -- the two spellings
 * host languages differ on. An empty name is [DEFAULT_LOG_LEVEL].
 */
fun parseLogLevel(name: String): String? {
    if (name.isEmpty()) return DEFAULT_LOG_LEVEL
    val lowered = name.lowercase()
    if (lowered == "warn") return "warning"
    if (lowered == "fatal") return "critical"
    return if (lowered in LOG_LEVELS) lowered else null
}

/**
 * Everything about a log other than the value being logged.
 *
 * [metadata] is merged onto the chunk first and the named fields after it, so an
 * explicit [level] wins over a `"level"` the same caller also put in the map.
 */
data class LogOptions(
    val level: String = "",
    /**
     * Media type hint for the serializer.
     *
     * Unlike C++, Kotlin distinguishes text from bytes already: a `String` is
     * `text/plain` and a `ByteArray` is `application/octet-stream` through the
     * ordinary registry, so a log needs no special case here.
     */
    val mimetype: String = "",
    val channel: String = "",
    val file: String = "",
    val lineno: Int? = null,
    val internal: Boolean = false,
    val metadata: Map<String, ByteArray>? = null,
)

/** One log as a sink sees it. */
data class LogRecord(
    val actionName: String,
    val actionId: String,
    val level: String,
    val channel: String,
    val file: String,
    val lineno: Int?,
    val internal: Boolean,
    val mimetype: String,
    val data: ByteArray,
    val timestampMillis: Long?,
)

/** What the process does with a log it consumes itself. */
typealias ActionLogSink = (LogRecord) -> Unit

/**
 * Whether a log payload of this media type reads as a line of characters.
 *
 * Text and JSON do, so both print as themselves. Anything else is bytes, and a
 * log line is not the place to render a blob -- [logText] describes it instead,
 * and the payload is still on the record for a sink that wants it.
 */
fun isTextualLogMimetype(mimetype: String): Boolean {
    if (mimetype.startsWith("text/")) return true
    val media = mimetype.substringBefore(';')
    return media == JSON_MIMETYPE || media.endsWith("+json")
}

/** A record as one line: its payload where that is text, a description if not. */
fun logText(record: LogRecord): String =
    if (isTextualLogMimetype(record.mimetype)) String(record.data, Charsets.UTF_8)
    else "<${record.data.size} bytes of ${record.mimetype}>"

/** The default sink: standard error, with the level and the action in front. */
private fun reportThroughStderr(record: LogRecord) {
    val where =
        if (record.channel.isEmpty()) "[${record.actionName}]"
        else "[${record.actionName}/${record.channel}]"
    System.err.println("${record.level.uppercase()} $where ${logText(record)}")
}

@Volatile private var installedSink: ActionLogSink? = null

/**
 * Install [next] as the process's action log sink.
 *
 * One slot rather than one sink per interested party, so a caller that takes it
 * takes it from whoever had it -- which is what keeps a log from being reported
 * twice. `null` restores the default.
 */
fun setActionLogSink(next: ActionLogSink?) {
    installedSink = next
}

/** The installed sink, or the default. Never null. */
fun getActionLogSink(): ActionLogSink = installedSink ?: ::reportThroughStderr

/** Report [record] to the installed sink. Never throws. */
fun reportLog(record: LogRecord) {
    try {
        getActionLogSink()(record)
    } catch (_: Throwable) {
        // A failure to log must never reach the code that was logging.
    }
}

private fun attribute(chunk: Chunk, key: String): String {
    val value = chunk.metadata?.attributes?.get(key) ?: return ""
    return String(value, Charsets.UTF_8)
}

/**
 * Read a [LogRecord] back out of a log chunk.
 *
 * The inverse of what [Action.log] writes, so a consumer on the far end of a wire
 * reads the metadata the same way every other language does. A missing or unknown
 * level falls back to [DEFAULT_LOG_LEVEL].
 */
fun logRecordFromChunk(chunk: Chunk, actionName: String = "", actionId: String = ""): LogRecord =
    LogRecord(
        actionName = actionName,
        actionId = actionId,
        level = parseLogLevel(attribute(chunk, LOG_LEVEL_ATTRIBUTE)) ?: DEFAULT_LOG_LEVEL,
        channel = attribute(chunk, LOG_CHANNEL_ATTRIBUTE),
        file = attribute(chunk, LOG_FILE_ATTRIBUTE),
        lineno = attribute(chunk, LOG_LINENO_ATTRIBUTE).toIntOrNull(),
        internal = attribute(chunk, LOG_INTERNAL_ATTRIBUTE) == LOG_INTERNAL_TRUE,
        mimetype = chunk.metadata?.mimetype ?: "",
        data = chunk.data,
        timestampMillis = chunk.metadata?.timestampMillis,
    )
