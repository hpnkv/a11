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

/** Scheme-labelled credential fields interpreted by a host storage resolver. */
data class StorageCredentials(val scheme: String, val fields: Map<String, String> = emptyMap()) {
    override fun toString(): String = "StorageCredentials(<redacted>)"
}

/** One named service participating in a storage backend. */
data class StorageService(
    val role: String,
    val endpoint: String = "",
    val credentials: StorageCredentials? = null,
)

/** Logical namespace with optional backend access information. */
data class StorageIdentity(
    val id: String,
    val services: List<StorageService> = emptyList(),
    val credentials: StorageCredentials? = null,
) {
    internal fun toFields(): Map<String, Any?> =
        mapOf("id" to id) +
            (credentials?.let {
                mapOf("credentials" to mapOf("scheme" to it.scheme, "fields" to it.fields))
            } ?: emptyMap()) +
            if (services.isEmpty()) emptyMap()
            else
                mapOf(
                    "services" to
                        services.map { service ->
                            mapOf("role" to service.role, "endpoint" to service.endpoint) +
                                (service.credentials?.let {
                                    mapOf(
                                        "credentials" to
                                            mapOf("scheme" to it.scheme, "fields" to it.fields)
                                    )
                                } ?: emptyMap())
                        }
                )

    override fun toString(): String = "StorageIdentity(id='$id', services=${services.size})"

    companion object {
        internal fun fromValue(value: Any?): StatusOr<StorageIdentity> {
            val fields = value as? Map<*, *> ?: return invalidArgument("Invalid storage identity.")
            val id = fields["id"] as? String ?: return invalidArgument("Invalid storage id.")
            val services = mutableListOf<StorageService>()
            val entries = fields["services"] ?: emptyList<Any?>()
            if (entries !is List<*>) return invalidArgument("Invalid storage services.")
            for (entry in entries) {
                val service =
                    entry as? Map<*, *> ?: return invalidArgument("Invalid storage service.")
                val role =
                    service["role"] as? String ?: return invalidArgument("Invalid storage role.")
                val endpoint = service["endpoint"] as? String ?: ""
                var credentials: StorageCredentials? = null
                val raw = service["credentials"]
                if (raw != null) {
                    if (raw !is Map<*, *>) return invalidArgument("Invalid storage credentials.")
                    val scheme =
                        raw["scheme"] as? String
                            ?: return invalidArgument("Invalid storage scheme.")
                    val values = raw["fields"] ?: emptyMap<String, String>()
                    if (values !is Map<*, *>) return invalidArgument("Invalid credential fields.")
                    val attributes = LinkedHashMap<String, String>()
                    for ((key, item) in values) {
                        if (key !is String || item !is String)
                            return invalidArgument("Credential fields must be strings.")
                        attributes[key] = item
                    }
                    credentials = StorageCredentials(scheme, attributes)
                }
                services.add(StorageService(role, endpoint, credentials))
            }
            var namespaceAccess: StorageCredentials? = null
            val rawNamespace = fields["credentials"]
            if (rawNamespace != null) {
                val access =
                    rawNamespace as? Map<*, *>
                        ?: return invalidArgument("Invalid namespace credentials.")
                val scheme =
                    access["scheme"] as? String
                        ?: return invalidArgument("Invalid namespace scheme.")
                val values = access["fields"] ?: emptyMap<String, String>()
                if (values !is Map<*, *>) return invalidArgument("Invalid credential fields.")
                val attributes = LinkedHashMap<String, String>()
                for ((key, item) in values) {
                    if (key !is String || item !is String)
                        return invalidArgument("Credential fields must be strings.")
                    attributes[key] = item
                }
                namespaceAccess = StorageCredentials(scheme, attributes)
            }
            return Ok(StorageIdentity(id, services, namespaceAccess))
        }
    }
}
