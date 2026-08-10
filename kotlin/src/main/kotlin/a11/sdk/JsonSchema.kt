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
