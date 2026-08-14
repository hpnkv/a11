# Copyright 2026 The A11 Authors.

"""Syntax highlighting for the A11 Flow language, as a Pygments lexer.

GENERATED FILE -- do not edit it by hand. It is written by
``a11 flow syntax --target pygments --generate`` (or ``a11-flow syntax ...``)
from the language's own word tables, and ``--check`` holds it to being up to
date. A
word added to the language reaches this file by running the generator; edited
here, it would be overwritten and the drift would be silent.

This is what colours the flows in A11's own documentation: MkDocs highlights a
fenced block with Pygments, and ``doc/hooks/flow_highlighting.py`` registers
this lexer under the alias ``a11flow``. Nothing about it is specific to that,
though -- it is an ordinary Pygments lexer, so Sphinx, ``pygmentize`` and a
static site of
your own can all read a ``.flow`` file through it.

Every keyword may be written in lower case or UPPER CASE, but not Mixed, which
is what ``_keywords`` builds the two spellings of: ``For`` highlights as a name
just as the compiler reads it as one.
"""

from pygments.lexer import RegexLexer, bygroups, default, include
from pygments.token import (
    Comment,
    Keyword,
    Name,
    Number,
    Operator,
    Punctuation,
    String,
    Text,
    Whitespace,
)

__all__ = ["A11FlowLexer"]

#: A name: letters, digits, underscores, and dashes between word characters.
NAME = r"[A-Za-z_$][A-Za-z0-9_$]*(?:-[A-Za-z0-9_$]+)*"

#: What must follow a keyword for it to be one, rather than the first part of a
#: longer name: ``in`` is a direction and ``inputs`` is not.
BOUNDARY = r"(?![A-Za-z0-9_$-])"

#: A dotted name -- a tag a serialisation registry knows a type by, or the type
#: of a value being built: ``a11.sdk.AudioBuffer``.
DOTTED = NAME + r"(?:\." + NAME + r")"


def _keywords(*names):
    """``flow|FLOW``: the two spellings of each word, as one alternation.

    A two-word entry keeps its space as ``\\s+``, so ``one of`` matches however
    it is spaced.
    """
    spellings = [*names, *(name.upper() for name in names)]
    return "|".join(name.replace(" ", r"\s+") for name in spellings)


def _group(alternation):
    """A whole-word group around an alternation: what a rule matches."""
    return r"\b(" + alternation + r")" + BOUNDARY


def _word(*names):
    """The pattern for the keywords named, in either spelling."""
    return _group(_keywords(*names))


#: The directions a port is declared in.
DIRECTIONS = _keywords("in", "out")

#: What a port says about itself after its type.
PORT_MODIFIERS = _keywords("stream", "required")

#: What a ``struct`` field says about itself after its type.
FIELD_MODIFIERS = _keywords(
    "required", "unique", "matching", "one of", "default"
)

#: The built-in port type names.
TYPES = _keywords(
    "string", "text", "number", "integer", "int", "bool", "boolean",
    "duration", "time", "object", "json", "list", "array", "bytes", "any"
)

#: What may follow a ``header``.
HEADER_WORDS = _keywords("as", "default")

#: The statements that may follow a ``=``, which is what makes the name before
#: it a bound step rather than one side of a comparison.
BINDING_VERBS = _keywords("run", "call", "try", "node", "wait", "drain")

#: Words that open a statement.
STATEMENTS = _keywords(
    "let", "advance", "skip", "wait", "drain", "cancel", "fail", "if",
    "for", "repeat", "until", "while", "else"
)

#: Words that stand inside a statement without opening one.
CLAUSES = _keywords("parallel", "max")

#: What may follow a call's closing parenthesis.
MODIFIERS = _keywords(
    "tee", "via", "timeout", "after", "with", "id", "forward", "headers"
)

#: Every pipeline stage.
STAGES = _keywords(
    "first", "last", "drop", "truncate", "batch", "group", "where", "map",
    "match", "distinct", "then", "mime", "strformat", "chunk", "collect",
    "count", "join", "text", "json", "packb"
)

#: The stages that may be written without their leading ``|``.
BARE_STAGES = _keywords("where", "then")

#: Words that open a pipeline source rather than naming one.
SOURCE_WORDS = _keywords("status", "zip")

#: Operators that are words.
OPERATOR_WORDS = _keywords("and", "or", "not", "in")

#: The language's fixed function set. No user code, ever: a flow stays data.
BUILTINS = _keywords(
    "len", "lower", "upper", "trim", "text", "number", "bool", "keys",
    "values", "get", "join", "split", "merge", "contains", "starts-with",
    "ends-with", "replace", "match", "slice", "default", "to_chunk",
    "from_chunk", "strformat", "b64encode", "b64decode", "b64urlencode",
    "b64urldecode", "now", "duration", "time", "seconds"
)

#: The canonical status codes, which is what ``fail`` names.
STATUS_CODES = _keywords(
    "ok", "cancelled", "unknown", "invalid_argument", "deadline_exceeded",
    "not_found", "already_exists", "permission_denied",
    "resource_exhausted", "failed_precondition", "aborted",
    "out_of_range", "unimplemented", "internal", "unavailable",
    "data_loss", "unauthenticated"
)

#: Literals that are words.
CONSTANTS = _keywords("true", "false", "null")

#: Duration suffixes a number may carry, longest spelling first: a pattern that
#: offered ``m`` before ``ms`` would read ``250ms`` as a number of metres.
DURATION_UNITS = r"ns|us|ms|s|m|h"


class A11FlowLexer(RegexLexer):
    """Lexer for the A11 Flow language.

    A word of Flow means what its position says it means -- there are no
    reserved words -- so the states below are mostly about position: a stage
    only follows a ``|``, a type only follows a port's ``:``, a function is only
    a function where it is called, and whatever follows a ``.`` is a member
    however it is spelled.
    """

    name = "A11 Flow"
    url = "https://github.com/hpnkv/a11"
    aliases = ["a11flow", "a11-flow"]
    filenames = ["*.flow"]
    mimetypes = ["text/x-a11flow"]

    tokens = {
        "root": [
            (r"[^\S\n]+", Whitespace),
            (r"\n", Whitespace),
            include("comment"),
            include("string"),
            (_word("flow"), Keyword.Declaration, "flow-name"),
            (_word("struct"), Keyword.Declaration, "struct-name"),
            # A port, told from the `in` of `x in y` by what follows it.
            (
                _group(DIRECTIONS) + r"(?=\s+" + NAME + r"\s*:)",
                Keyword.Declaration,
                "port",
            ),
            (_word("header"), Keyword.Declaration, "header"),
            (_word("describe"), Keyword.Declaration),
            (_word("nodes"), Keyword.Declaration, "node-map"),
            # Making a node takes parentheses -- `node()`, `node(id)` -- so the
            # word is the keyword only where one opens, and a port called `node`
            # is a name.
            (_word("node") + r"(?=\s*\()", Keyword.Declaration),
            # `x = run ...`: the name before the `=` is the step being bound,
            # and a step is coloured the way it is coloured where it is used
            # again -- `mic` and the `mic` of `mic.audio` are the same thing.
            (
                r"(" + NAME + r")(\s*)(=)(?=\s*(?:" + BINDING_VERBS + r")"
                + BOUNDARY + r")",
                bygroups(Name.Variable, Whitespace, Operator),
            ),
            # `state <- source`: what a repeat carries.
            (
                r"(" + NAME + r")(\s*)(<-)",
                bygroups(Name.Variable, Whitespace, Operator),
            ),
            (_word("try"), Keyword),
            (_word("run", "call"), Keyword, "action-name"),
            # `via scratch` names a node map, so it is coloured as one -- the
            # same name the `nodes` that declared it was given.
            (
                r"(" + _keywords("via") + r")" + BOUNDARY + r"([^\S\n]+)("
                + NAME + r")",
                bygroups(Keyword.Reserved, Whitespace, Name.Namespace),
            ),
            (_group(MODIFIERS), Keyword.Reserved),
            (_group(STATEMENTS), Keyword),
            (_group(CLAUSES), Keyword),
            # A stage, which is what a word after a `|` is. The gap may hold a
            # line break: a long pipeline is written one stage to a line.
            (
                r"(\|)(\s*)(" + STAGES + r")" + BOUNDARY,
                bygroups(Operator, Whitespace, Name.Builtin.Pseudo),
            ),
            # The two stages that may be written without their `|` read as words
            # joining two things -- `history then asked`, `hits where it.ok`.
            # Both take an operand, which is what tells the stage from a port of
            # the same name.
            (
                _group(BARE_STAGES)
                + r"(?=[^\S\n]+(?:[A-Za-z_$\"(\[{]|[0-9]|-[0-9]))",
                Name.Builtin.Pseudo,
            ),
            (_word("as"), Keyword, "cast"),
            # `a11.sdk.Interaction{...}`: a value of a named type.
            (DOTTED + r"+(?=\s*\{)", Keyword.Type),
            (_group(SOURCE_WORDS), Keyword),
            (_word("it"), Name.Builtin.Pseudo),
            (_group(OPERATOR_WORDS), Operator.Word),
            (_group(BUILTINS) + r"(?=\s*\()", Name.Builtin),
            (_group(STATUS_CODES), Name.Constant),
            include("literal"),
            include("operator"),
            include("name"),
            (r".", Text),
        ],
        "comment": [
            (r"#[^\n]*", Comment.Single),
        ],
        "string": [
            # `"""..."""` first, so three quotes are not read as
            # an empty string and a quote. A line break inside one is content,
            # which is the whole point of it.
            (r'"""', String, "block-string"),
            (r'"', String, "quoted-string"),
        ],
        "block-string": [
            (r'\\.', String.Escape),
            (r'"""', String, "#pop"),
            (r'[^\\"]+', String),
            (r'"', String),
        ],
        "quoted-string": [
            (r'\\.', String.Escape),
            (r'"', String, "#pop"),
            (r'[^\\"\n]+', String),
            (r"\n", Whitespace, "#pop"),
        ],
        "flow-name": [
            (r"[^\S\n]+", Whitespace),
            (NAME, Name.Function, "#pop"),
            (r'"', String, ("#pop", "quoted-string")),
            default("#pop"),
        ],
        "struct-name": [
            (r"[^\S\n]+", Whitespace),
            (NAME, Name.Class, ("#pop", "struct-body")),
            default("#pop"),
        ],
        # A struct's body is fields and nothing else, which is why it is its own
        # state rather than a use of `root`.
        "struct-body": [
            (r"[^\S\n]+", Whitespace),
            (r"\n", Whitespace),
            (r"\{", Punctuation),
            (r"\}", Punctuation, "#pop"),
            include("comment"),
            (_word("describe"), Keyword.Declaration),
            include("string"),
            (
                r"(" + NAME + r")(\s*)(:)",
                bygroups(Name.Attribute, Whitespace, Punctuation),
                "field-type",
            ),
            (r".", Text),
        ],
        "field-type": [
            (r"[^\S\n]+", Whitespace),
            (_group(FIELD_MODIFIERS), Keyword.Pseudo),
            include("type"),
            include("string"),
            include("literal"),
            # `1..200`: the range between two bounds.
            (r"\.\.", Operator),
            (r"[\[\],]", Punctuation),
            (r"\n", Whitespace, "#pop"),
            (r".", Text),
        ],
        "port": [
            (r"[^\S\n]+", Whitespace),
            (NAME, Name.Attribute),
            (r":", Punctuation, ("#pop", "port-type")),
            default("#pop"),
        ],
        "port-type": [
            (r"[^\S\n]+", Whitespace),
            (_group(PORT_MODIFIERS), Keyword.Pseudo),
            include("type"),
            include("string"),
            # The brackets a generic type says what it holds in.
            (r"[\[\],]", Punctuation),
            (r"\n", Whitespace, "#pop"),
            (r".", Text),
        ],
        "header": [
            (r"[^\S\n]+", Whitespace),
            (_group(HEADER_WORDS), Keyword),
            include("string"),
            include("literal"),
            (NAME, Name.Variable),
            (r"\n", Whitespace, "#pop"),
            (r".", Text),
        ],
        "node-map": [
            (r"[^\S\n]+", Whitespace),
            (NAME, Name.Namespace, "#pop"),
            default("#pop"),
        ],
        "action-name": [
            (r"[^\S\n]+", Whitespace),
            (NAME + r"(?:\." + NAME + r")*", Name.Function, "#pop"),
            (r'"', String, ("#pop", "quoted-string")),
            default("#pop"),
        ],
        # `expr as TYPE`, and the type it names -- which may be a registry tag,
        # so it is read by shape rather than looked up in a list.
        "cast": [
            (r"[^\S\n]+", Whitespace),
            (
                NAME + r"(?:\." + NAME + r")*(?:\s*\[[^\]\n]*\])?",
                Keyword.Type,
                "#pop",
            ),
            (r'"', String, ("#pop", "quoted-string")),
            default("#pop"),
        ],
        "type": [
            # A tag first, so one whose first part happens to be a built-in name
            # is still read as the whole tag.
            (DOTTED + r"+", Keyword.Type),
            (_group(TYPES), Keyword.Type),
        ],
        "literal": [
            (_group(CONSTANTS), Keyword.Constant),
            (r"-?\d+(?:\.\d+)?(?:" + DURATION_UNITS + r")" + BOUNDARY, Number),
            (r"-?\d+(?:\.\d+)?", Number),
        ],
        "operator": [
            (r"->|<-", Operator),
            (r"==|!=|<=|>=", Operator),
            (r"\.\.", Operator),
            (r"[<>|=+*/%-]", Operator),
            (r"[{}()\[\]]", Punctuation),
            (r"[:,.]", Punctuation),
        ],
        "name": [
            # `x.port` -- a call's port, a node's id, a field of a value.
            (NAME + r"(?=\s*\.)", Name.Variable),
            (r"(?<=\.)" + NAME, Name.Attribute),
            (NAME, Name),
        ],
    }
