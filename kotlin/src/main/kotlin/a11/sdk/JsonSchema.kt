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

package a11.sdk

/**
 * JSON-Schema helpers used when translating A11 actions into model tools.
 *
 * The full TypeScript `organiseAndDeduplicateJsonschema` hoists `$defs` and
 * deduplicates repeated subschemas. A11 action port schemas derived from MIME
 * types are flat (no `$defs`/`$ref`), so this port passes them through; the hook
 * is kept so richer per-port schemas can be deduplicated later.
 */
fun organiseAndDeduplicateJsonschema(schema: Map<String, Any?>): Map<String, Any?> = schema

/** Derive a JSON-Schema type from a port MIME type: a text media type maps to string, else object. */
fun mimeToJsonSchema(mimetype: String): Map<String, Any?> {
    val mediaType = mimetype.split(";").firstOrNull()?.trim()?.lowercase() ?: ""
    return if (mediaType.startsWith("text/")) mapOf("type" to "string") else mapOf("type" to "object")
}
