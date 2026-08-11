package dev.curiositystack.a11.clion.flow

/**
 * The words the A11 Flow language gives meaning to.
 *
 * This is a second copy of tables that live in `a11/flow/parser.py` and
 * `a11/flow/plan.py`, which is exactly the kind of copy that drifts. It is
 * written as plain `setOf` literals so the repository's
 * `a11/flow/tests/test_editor_support.py` can read it back and fail when the
 * language grows a word the editor was not told about.
 *
 * Every one of these may be written in lower case or UPPER CASE, and only in
 * those: a uniformly-cased word is a keyword, and `For` is a name. That is the
 * compiler's rule, and [canonical] is the same rule here.
 */
object FlowVocabulary {

    /** Words that declare something: a flow, a port, a header, a node. */
    val DECLARATION_WORDS = setOf(
        "flow", "describe", "in", "out", "header", "as", "default",
        "required", "stream", "node", "nodes",
    )

    /**
     * Words that open a statement.
     *
     * `run` and `call` are both here because they are the two dispatch verbs:
     * `run` executes the handler registered where the flow runs, `call` puts
     * the action on the stream the flow is attached to.
     */
    val STATEMENT_WORDS = setOf(
        "run", "call", "try", "skip", "wait", "drain", "cancel", "fail", "for",
        "repeat", "until", "while", "if", "else", "status", "parallel", "max",
    )

    /**
     * Words that may follow a call's closing parenthesis.
     *
     * `headers` is only ever the second word of `forward headers "x-name"`,
     * which sends the flow's own headers on to the step without naming a value
     * for each; it is a modifier word so that a line beginning with it still
     * reads as a continuation of the call above.
     */
    val MODIFIER_WORDS = setOf(
        "tee", "via", "timeout", "after", "with", "id", "forward", "headers",
    )

    /** Pipeline stages, which are stages directly after a `|`. */
    val STAGE_WORDS = setOf(
        "first", "last", "drop", "truncate", "batch", "group", "then",
        "where", "map", "join", "strformat", "mime", "collect", "count",
        "distinct", "text", "json", "packb",
    )

    /**
     * The two stages that may be written without their `|`.
     *
     * `history then asked` and `hits where it.ok` read as words joining two
     * things rather than as a transformation applied to a stream, so the pipe
     * is optional in front of them. Both take an operand, and that is what
     * tells the stage from a port of the same name -- see
     * `Parser._at_bare_stage`, whose rule [FlowLexer] repeats.
     */
    val BARE_STAGE_WORDS = setOf("then", "where")

    /** The fixed function set an expression may call. */
    val BUILTIN_WORDS = setOf(
        "len", "lower", "upper", "trim", "text", "number", "bool", "keys",
        "values", "get", "join", "split", "merge", "contains", "starts-with",
        "ends-with", "replace", "slice", "default", "to_chunk", "from_chunk",
        "strformat", "now", "duration", "time", "seconds",
    )

    /** What a port says about itself *after* its type. */
    val PORT_MODIFIER_WORDS = setOf("stream", "required")

    /**
     * The built-in port types.
     *
     * Not the whole of what may stand where a type does: a port may also name a
     * type by the tag a serialisation registry knows it by --
     * `a11.sdk.AudioBuffer` -- or say what a generic one holds, as in
     * `list[a11.NodeFragment]`. Those are read from *where* they are rather
     * than from a list, which is [FlowLexer]'s business.
     */
    val TYPE_WORDS = setOf(
        "string", "text", "number", "integer", "int", "bool", "boolean",
        "object", "json", "list", "array", "bytes", "any",
    )

    /** Abseil's canonical status codes, which is what `fail` names. */
    val STATUS_CODES = setOf(
        "ok", "cancelled", "unknown", "invalid_argument", "deadline_exceeded",
        "not_found", "already_exists", "permission_denied",
        "resource_exhausted", "failed_precondition", "aborted", "out_of_range",
        "unimplemented", "internal", "unavailable", "data_loss",
        "unauthenticated",
    )

    /** Literals that are words. */
    val CONSTANT_WORDS = setOf("true", "false", "null", "it")

    /** Operators that are words. */
    val OPERATOR_WORDS = setOf("and", "or", "not")

    /** Duration suffixes a number may carry, down to the nanosecond. */
    val DURATION_UNITS = setOf("ns", "us", "ms", "s", "m", "h")

    /**
     * The lower-case form of a uniformly-cased word, or the word unchanged.
     *
     * The same rule as `a11.flow.lexer.canonical`: `FOR` and `for` are the
     * keyword, `For` is a name.
     */
    fun canonical(word: String): String =
        if (word.any { it.isUpperCase() } && word.none { it.isLowerCase() }) {
            word.lowercase()
        } else {
            word
        }
}
