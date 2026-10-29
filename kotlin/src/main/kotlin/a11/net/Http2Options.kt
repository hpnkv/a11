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

package a11.net

import a11.Status
import a11.Time
import a11.internal.NativeBridge
import a11.internal.nativeBoolean
import a11.internal.nativeText
import a11.value

private val httpDefaults by lazy { NativeBridge.fields(NativeBridge.call("http.defaults").value()) }
private val tlsDefaults by lazy { NativeBridge.fields(httpDefaults["tls"]) }

/** Native TLS certificate and peer-verification policy. */
data class Http2TlsOptions(
    val enabled: Boolean = nativeBoolean(tlsDefaults["enabled"]),
    val verifyPeer: Boolean = nativeBoolean(tlsDefaults["verify_peer"]),
    val certificatePemFile: String = nativeText(tlsDefaults["certificate_pem_file"]),
    val keyPemFile: String = nativeText(tlsDefaults["key_pem_file"]),
    val caCertificatePemFile: String = nativeText(tlsDefaults["ca_certificate_pem_file"]),
) {
    internal fun fields(): Map<String, Any?> =
        mapOf(
            "enabled" to enabled,
            "verify_peer" to verifyPeer,
            "certificate_pem_file" to certificatePemFile,
            "key_pem_file" to keyPemFile,
            "ca_certificate_pem_file" to caCertificatePemFile,
        )
}

/** HTTP transport limits and negotiation, with defaults read from C++. */
data class Http2Options(
    val maxRequestBodySize: Long = NativeBridge.number(httpDefaults["max_request_body_size"]),
    val maxResponseBodySize: Long = NativeBridge.number(httpDefaults["max_response_body_size"]),
    val maxBufferedRequestBytes: Long =
        NativeBridge.number(httpDefaults["max_buffered_request_bytes"]),
    val maxBufferedResponseBytes: Long =
        NativeBridge.number(httpDefaults["max_buffered_response_bytes"]),
    val deadline: Time? = null,
    val tls: Http2TlsOptions = Http2TlsOptions(),
    val enableH2: Boolean = nativeBoolean(httpDefaults["enable_h2"]),
    val enableH2c: Boolean = nativeBoolean(httpDefaults["enable_h2c"]),
    val enableHttp1: Boolean = nativeBoolean(httpDefaults["enable_http1"]),
    val enablePush: Boolean = nativeBoolean(httpDefaults["enable_push"]),
    val clientPreference: ProtocolPreference =
        ProtocolPreference.entries[NativeBridge.number(httpDefaults["client_preference"]).toInt()],
    val clientAllowDowngrade: Boolean = nativeBoolean(httpDefaults["client_allow_downgrade"]),
) {
    enum class ProtocolPreference {
        AUTO,
        HTTP2,
        HTTP11,
    }

    internal fun fields(): Map<String, Any?> =
        mapOf(
            "max_request_body_size" to maxRequestBodySize,
            "max_response_body_size" to maxResponseBodySize,
            "max_buffered_request_bytes" to maxBufferedRequestBytes,
            "max_buffered_response_bytes" to maxBufferedResponseBytes,
            "deadline" to deadline,
            "tls" to tls.fields(),
            "enable_h2" to enableH2,
            "enable_h2c" to enableH2c,
            "enable_http1" to enableHttp1,
            "enable_push" to enablePush,
            "client_preference" to clientPreference.ordinal,
            "client_allow_downgrade" to clientAllowDowngrade,
        )

    fun validate(): Status =
        NativeBridge.status("http.validateOptions", args = mapOf("http2Options" to fields()))
}
