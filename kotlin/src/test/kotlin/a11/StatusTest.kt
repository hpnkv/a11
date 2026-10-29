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

package a11

import java.nio.file.Path
import java.util.concurrent.TimeUnit
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

class StatusTest {
    @Test
    fun canonicalCodesAndProtocolMappingsMatchNative() {
        assertEquals((0..16).toList(), StatusCode.entries.map { it.value })
        val http =
            listOf(
                200,
                499,
                500,
                400,
                504,
                404,
                409,
                403,
                429,
                400,
                409,
                400,
                501,
                500,
                503,
                500,
                401,
            )
        val websocket =
            listOf(
                1000,
                4000,
                4001,
                4002,
                4003,
                4004,
                4005,
                4006,
                4007,
                4008,
                4009,
                4010,
                4011,
                4012,
                4013,
                4014,
                4007,
            )
        // The native encoder and decoder deliberately have asymmetric tables.
        val decoded =
            listOf(
                StatusCode.CANCELLED,
                StatusCode.UNKNOWN,
                StatusCode.INVALID_ARGUMENT,
                StatusCode.DEADLINE_EXCEEDED,
                StatusCode.NOT_FOUND,
                StatusCode.ALREADY_EXISTS,
                StatusCode.PERMISSION_DENIED,
                StatusCode.UNAUTHENTICATED,
                StatusCode.RESOURCE_EXHAUSTED,
                StatusCode.FAILED_PRECONDITION,
                StatusCode.ABORTED,
                StatusCode.OUT_OF_RANGE,
                StatusCode.UNIMPLEMENTED,
                StatusCode.INTERNAL,
                StatusCode.UNAVAILABLE,
                StatusCode.DATA_LOSS,
            )
        for (code in StatusCode.entries) {
            assertEquals(code, StatusCode.fromValue(code.value))
            assertEquals(http[code.value], statusCodeToHttp(code))
            assertEquals(websocket[code.value], statusCodeToWebSocket(code))
        }
        for ((offset, code) in decoded.withIndex()) assertEquals(
            code,
            statusCodeFromWebSocket(4000 + offset),
        )
        assertEquals(StatusCode.OK, statusCodeFromWebSocket(1000))
        assertEquals(StatusCode.ABORTED, statusCodeFromWebSocket(1001))
        for (raw in listOf(1002, 1003, 1007)) assertEquals(
            StatusCode.INVALID_ARGUMENT,
            statusCodeFromWebSocket(raw),
        )
        assertEquals(StatusCode.PERMISSION_DENIED, statusCodeFromWebSocket(1008))
        assertEquals(StatusCode.RESOURCE_EXHAUSTED, statusCodeFromWebSocket(1009))
        assertEquals(StatusCode.INTERNAL, statusCodeFromWebSocket(1011))
        for (raw in listOf(1012, 1013)) assertEquals(
            StatusCode.UNAVAILABLE,
            statusCodeFromWebSocket(raw),
        )
        assertEquals(StatusCode.UNKNOWN, statusCodeFromWebSocket(4016))
        assertNull(StatusCode.fromValue(-1))
        assertNull(StatusCode.fromValue(17))
        for (raw in listOf(200, 204, 299)) assertEquals(StatusCode.OK, statusCodeFromHttp(raw))
        for ((raw, code) in
            listOf(
                400 to StatusCode.INVALID_ARGUMENT,
                401 to StatusCode.UNAUTHENTICATED,
                403 to StatusCode.PERMISSION_DENIED,
                404 to StatusCode.NOT_FOUND,
                409 to StatusCode.ABORTED,
                429 to StatusCode.RESOURCE_EXHAUSTED,
                501 to StatusCode.UNIMPLEMENTED,
                503 to StatusCode.UNAVAILABLE,
                418 to StatusCode.FAILED_PRECONDITION,
                599 to StatusCode.INTERNAL,
                302 to StatusCode.UNKNOWN,
            )) {
            assertEquals(code, statusCodeFromHttp(raw))
        }
    }

    @Test
    fun successIsCanonicalAndErrorEqualityExcludesLocalCause() {
        val ok = Status(StatusCode.OK, "ignored", listOf(mapOf("ignored" to true)))
        assertTrue(ok.ok())
        assertEquals("", ok.message)
        assertTrue(ok.details.isEmpty())
        assertEquals(Status.ok(), ok)
        assertEquals(Status.ok().hashCode(), ok.hashCode())
        assertEquals("OK", ok.toString())
        val first = Status(StatusCode.NOT_FOUND, "missing", cause = Exception("local"))
        val second = notFound("missing")
        assertFalse(first.ok())
        assertEquals(first, second)
        assertEquals(first.hashCode(), second.hashCode())
        assertEquals("NOT_FOUND: missing", second.toString())
    }

    @Test
    fun detailsAreOwnedAndParticipateInEquality() {
        val nested = mutableListOf<Any?>("disk")
        val fields = mutableMapOf<String, Any?>("reasons" to nested)
        val status = Status(StatusCode.DATA_LOSS, "failed", listOf(fields))
        nested.clear()
        fields.clear()
        assertEquals(listOf(mapOf("reasons" to listOf("disk"))), status.details)
        assertEquals(status, Status(StatusCode.DATA_LOSS, "failed", status.details))
        assertTrue(status.toString().contains("disk"))
    }

    @Test
    fun valuesErrorsAndNullablePayloadsStayDistinct() {
        val payload = mutableListOf("value")
        val value: StatusOr<List<String>> = Ok(payload)
        assertSame(payload, value.value())
        assertTrue(value.ok())
        assertTrue(value.status.ok())
        assertEquals(Ok(5), value.map { it.single().length })
        val nullable: StatusOr<String?> = Ok(null)
        assertTrue(nullable.ok())
        assertNull(nullable.value())
        assertEquals(Ok("absent"), nullable.map { it ?: "absent" })
        val innerStatus = Ok(notFound("data"))
        assertTrue(innerStatus.ok())
        assertEquals(StatusCode.NOT_FOUND, innerStatus.value().code)
    }

    @Test
    fun propagationNeverReadsAnErrorValue() {
        val status =
            Status(StatusCode.PERMISSION_DENIED, "denied", listOf(mapOf("scope" to "tools")))
        val error: StatusOr<Int> = Err(status)
        assertFalse(error.ok())
        assertSame(status, error.status)
        assertSame(error, error.map { error("must not run") })
        assertSame(error, error.flatMap { error("must not run") })
        fun propagate(): StatusOr<String> {
            val value =
                error.orElse { status ->
                    return status
                }
            return Ok(value.toString())
        }
        assertSame(status, propagate().status)
        assertEquals(Ok("3"), Ok(3).flatMap { Ok(it.toString()) })
    }

    @Test
    fun statusAndValueResultsShareExplicitPropagation() {
        val failure = permissionDenied("preserved")
        fun propagate(status: Status): Status {
            status.orElse { status ->
                return status
            }
            return Status.ok()
        }
        assertSame(failure, propagate(failure))
        assertTrue(propagate(Status.ok()).ok())
        val success = Status.ok()
        assertSame(success, success.orElse { error("must not run") })
        val payload: StatusOr<Status> = Ok(failure)
        assertSame(failure, payload.orElse { error("successful Status payload") })
    }

    @Test
    fun fatalAccessAndInvalidConstructionTerminateAnIsolatedJvm() {
        for ((mode, diagnostic) in
            listOf("value" to "NOT_FOUND: subprocess missing", "ok-error" to "An OK status")) {
            val result = probe(mode)
            assertEquals(1, result.first, result.second)
            assertTrue(result.second.contains("FATAL:"), result.second)
            assertTrue(result.second.contains(diagnostic), result.second)
            assertFalse(result.second.contains("CAUGHT"), result.second)
            assertFalse(result.second.contains("SHUTDOWN"), result.second)
        }
        val inspected = probe("inspect")
        assertEquals(0, inspected.first, inspected.second)
        assertTrue(inspected.second.contains("INSPECTED"), inspected.second)
    }

    @Test
    fun numericDetailsRetainEqualityAndHashingAcrossTheWire() {
        val status = Status(StatusCode.RESOURCE_EXHAUSTED, "quota", listOf(mapOf("count" to 3)))
        val decoded = decodeStatus(packStatus(status).value()).value().status
        assertEquals(status, decoded)
        assertEquals(status.hashCode(), decoded.hashCode())
        assertEquals(status, status.copy())
        assertEquals("updated", status.copy(message = "updated").message)
        assertEquals(Status.ok(), status.copy(code = StatusCode.OK))
    }

    @Test
    fun directNativeDecoderRetainsNestedDetailsAndUnsignedIntegers() {
        val status =
            Status(
                StatusCode.DATA_LOSS,
                "native details",
                listOf(
                    mapOf(
                        "count" to java.math.BigInteger("18446744073709551615"),
                        "nested" to
                            listOf(null, true, 1.25, mapOf("message" to "text\u0000\uD83D\uDE00")),
                    )
                ),
            )
        val decoded = decodeStatus(packStatus(status).value()).value().status
        assertEquals(status, decoded)
        assertEquals(status.hashCode(), decoded.hashCode())
    }

    private fun probe(mode: String): Pair<Int, String> {
        val java = Path.of(System.getProperty("java.home"), "bin", "java").toString()
        val process =
            ProcessBuilder(
                    java,
                    "-cp",
                    System.getProperty("a11.test.classpath"),
                    "a11.StatusFatalProbe",
                    mode,
                )
                .redirectErrorStream(true)
                .start()
        try {
            assertTrue(process.waitFor(15, TimeUnit.SECONDS), "fatal probe did not exit")
            return process.exitValue() to process.inputStream.bufferedReader().readText()
        } finally {
            process.destroyForcibly()
        }
    }
}

/** Subprocess entry point; never loaded as a JUnit test. */
object StatusFatalProbe {
    @JvmStatic
    fun main(args: Array<String>) {
        Runtime.getRuntime().addShutdownHook(Thread { println("SHUTDOWN") })
        try {
            when (args.single()) {
                "value" -> notFound("subprocess missing").value()
                "ok-error" -> Err(Status.ok())
                "inspect" -> {
                    val result: StatusOr<String> = notFound("subprocess missing")
                    check(!result.ok() && result.status.code == StatusCode.NOT_FOUND)
                    check(result.map { error("must not run") } == result)
                    check(result.flatMap<String, String> { error("must not run") } == result)
                    println("INSPECTED")
                }
            }
        } catch (error: Throwable) {
            println("CAUGHT: $error")
        }
    }
}
