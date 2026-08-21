package a11

/**
 * Minimal JSON reader/writer over Kotlin `Any?` trees.
 *
 * Object keys preserve insertion order (LinkedHashMap); numbers decode to [Long]
 * when integral and in range, otherwise [Double]. Designed to interoperate
 * with the JSON A11 chunk codec, supporting objects, arrays, strings,
 * integers, numbers, booleans, and null.
 */
object A11Json {

    fun encodeToString(value: Any?): StatusOr<String> = try {
        val sb = StringBuilder()
        write(sb, value)
        Ok(sb.toString())
    } catch (error: Throwable) {
        invalidArgument("Failed to serialize JSON.")
    }

    fun parse(text: String): StatusOr<Any?> = try {
        val parser = Parser(text)
        val value = parser.parseValue()
        parser.skipWhitespace()
        if (!parser.atEnd()) invalidArgument("Trailing content after JSON value.") else Ok(value)
    } catch (error: Throwable) {
        invalidArgument("Invalid JSON data.")
    }

    private fun write(sb: StringBuilder, value: Any?) {
        when (value) {
            null -> sb.append("null")
            is Boolean -> sb.append(value.toString())
            is String -> writeString(sb, value)
            is Int -> sb.append(value.toString())
            is Long -> sb.append(value.toString())
            is Double -> sb.append(if (value % 1.0 == 0.0 && !value.isInfinite()) value.toLong().toString() else value.toString())
            is Float -> write(sb, value.toDouble())
            is Map<*, *> -> {
                sb.append('{')
                var first = true
                for ((k, v) in value) {
                    if (!first) sb.append(',')
                    first = false
                    writeString(sb, k.toString())
                    sb.append(':')
                    write(sb, v)
                }
                sb.append('}')
            }
            is List<*> -> {
                sb.append('[')
                var first = true
                for (item in value) {
                    if (!first) sb.append(',')
                    first = false
                    write(sb, item)
                }
                sb.append(']')
            }
            else -> throw IllegalArgumentException("Cannot JSON-encode ${value::class.java.name}.")
        }
    }

    private fun writeString(sb: StringBuilder, value: String) {
        sb.append('"')
        for (c in value) {
            when (c) {
                '"' -> sb.append("\\\"")
                '\\' -> sb.append("\\\\")
                '\n' -> sb.append("\\n")
                '\r' -> sb.append("\\r")
                '\t' -> sb.append("\\t")
                '\b' -> sb.append("\\b")
                '\u000C' -> sb.append("\\f")
                else -> if (c < ' ') sb.append("\\u%04x".format(c.code)) else sb.append(c)
            }
        }
        sb.append('"')
    }

    private class Parser(val text: String) {
        var pos = 0

        fun atEnd(): Boolean = pos >= text.length

        fun skipWhitespace() {
            while (pos < text.length && text[pos].isWhitespace()) pos++
        }

        fun parseValue(): Any? {
            skipWhitespace()
            return when (val c = text[pos]) {
                '{' -> parseObject()
                '[' -> parseArray()
                '"' -> parseString()
                't', 'f' -> parseBoolean()
                'n' -> parseNull()
                else -> if (c == '-' || c.isDigit()) parseNumber() else error("Unexpected char '$c'")
            }
        }

        private fun parseObject(): LinkedHashMap<String, Any?> {
            val result = LinkedHashMap<String, Any?>()
            pos++ // {
            skipWhitespace()
            if (text[pos] == '}') { pos++; return result }
            while (true) {
                skipWhitespace()
                val key = parseString()
                skipWhitespace()
                require(text[pos] == ':') { "Expected ':'" }
                pos++
                result[key] = parseValue()
                skipWhitespace()
                when (text[pos]) {
                    ',' -> pos++
                    '}' -> { pos++; return result }
                    else -> error("Expected ',' or '}'")
                }
            }
        }

        private fun parseArray(): ArrayList<Any?> {
            val result = ArrayList<Any?>()
            pos++ // [
            skipWhitespace()
            if (text[pos] == ']') { pos++; return result }
            while (true) {
                result.add(parseValue())
                skipWhitespace()
                when (text[pos]) {
                    ',' -> pos++
                    ']' -> { pos++; return result }
                    else -> error("Expected ',' or ']'")
                }
            }
        }

        private fun parseString(): String {
            require(text[pos] == '"') { "Expected string" }
            pos++
            val sb = StringBuilder()
            while (text[pos] != '"') {
                val c = text[pos]
                if (c == '\\') {
                    pos++
                    when (val e = text[pos]) {
                        '"' -> sb.append('"')
                        '\\' -> sb.append('\\')
                        '/' -> sb.append('/')
                        'n' -> sb.append('\n')
                        'r' -> sb.append('\r')
                        't' -> sb.append('\t')
                        'b' -> sb.append('\b')
                        'f' -> sb.append('\u000C')
                        'u' -> {
                            val hex = text.substring(pos + 1, pos + 5)
                            sb.append(hex.toInt(16).toChar())
                            pos += 4
                        }
                        else -> error("Invalid escape '\\$e'")
                    }
                } else {
                    sb.append(c)
                }
                pos++
            }
            pos++ // closing quote
            return sb.toString()
        }

        private fun parseNumber(): Any {
            val start = pos
            if (text[pos] == '-') pos++
            while (pos < text.length && (text[pos].isDigit() || text[pos] in ".eE+-")) pos++
            val token = text.substring(start, pos)
            val isFloat = token.any { it == '.' || it == 'e' || it == 'E' }
            if (!isFloat) {
                token.toLongOrNull()?.let { return it }
            }
            return token.toDouble()
        }

        private fun parseBoolean(): Boolean {
            return if (text.startsWith("true", pos)) { pos += 4; true }
            else { require(text.startsWith("false", pos)) { "Invalid literal" }; pos += 5; false }
        }

        private fun parseNull(): Any? {
            require(text.startsWith("null", pos)) { "Invalid literal" }
            pos += 4
            return null
        }
    }
}
