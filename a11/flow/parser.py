"""The Flow language parser: source text to a syntax tree.

The grammar is small enough to read in one sitting, and is written out in full
in [a11.flow][]'s module docstring. Recursive descent, one token of lookahead,
no keywords reserved outside the position that needs them -- a word only means
``skip`` or ``for`` when it opens a statement and is not immediately followed by
something that makes it a name.
"""

from __future__ import annotations

from typing import Any

from a11 import timing
from a11.flow import syntax
from a11.flow.lexer import FlowSyntaxError, Token, canonical, tokenize

#: Stage name -> what it takes: a number, an expression, a string, another
#: stream to read (``stream``), or nothing.
STAGES: dict[str, str | None] = {
    "first": "number",
    "last": "number",
    "drop": "number",
    "truncate": "number",
    "batch": "number",
    "where": "expr",
    "map": "expr",
    "join": "string?",
    "strformat": "string",
    "mime": "string",
    "group": "expr",
    "then": "stream",
    "collect": None,
    "count": None,
    "distinct": None,
    "text": None,
    "json": None,
    "packb": None,
}

#: Stages that may be written without the leading `|`. Both read as words
#: joining two things rather than as transformations applied to a stream --
#: `history then asked`, `hits where it.ok` -- and both take an operand, which
#: is what keeps them apart from a port of the same name.
BARE_STAGES = frozenset({"then", "where"})

#: Words that open a statement, and so are not read as a name there. Public
#: because a highlighter or an IDE plugin needs the same list.
STATEMENT_WORDS = frozenset(
    {
        "run",
        "call",
        "try",
        "skip",
        "wait",
        "drain",
        "cancel",
        "fail",
        "for",
        "repeat",
        "until",
        "while",
        "if",
        "nodes",
    }
)

#: Words that open a pipeline source, rather than naming one.
SOURCE_WORDS = frozenset({"status"})

#: Words that declare something inside a flow.
DECLARATION_WORDS = frozenset(
    {"flow", "describe", "in", "out", "header", "as", "default", "required",
     "stream", "node", "nodes"}
)

#: Words that may follow a call's closing parenthesis. ``headers`` is only ever
#: the second word of ``forward headers``, and is listed so a line beginning
#: with it still reads as a continuation of the call above.
MODIFIER_WORDS = frozenset(
    {"tee", "via", "timeout", "after", "with", "id", "forward", "headers"}
)

_COMPARISONS = frozenset({"==", "!=", "<", "<=", ">", ">="})

#: The language's fixed function set. No user code, ever -- a flow stays data.
BUILTINS = frozenset(
    {
        "len",
        "lower",
        "upper",
        "trim",
        "text",
        "number",
        "bool",
        "keys",
        "values",
        "get",
        "join",
        "split",
        "merge",
        "contains",
        "starts-with",
        "ends-with",
        "replace",
        "slice",
        "default",
        # Named after the Python functions they are: a flow that says
        # `to_chunk(x)` is doing what `a11.to_chunk(x)` does.
        "to_chunk",
        "from_chunk",
        # Formatting, without str.format's attribute walk. See
        # a11.flow.values.strformat.
        "strformat",
        # Times and durations. `now()` is the only builtin that is not a pure
        # function of its arguments, and it is worth the exception: a flow
        # cannot time itself otherwise. `duration` and `time` read what the
        # language writes -- `1m30s`, an RFC 3339 instant -- so a value that
        # arrived as text is a value like any other.
        "now",
        "duration",
        "time",
        "seconds",
    }
)


class Parser:
    """A recursive-descent parser over one Flow source file."""

    def __init__(self, source: str, source_name: str = "") -> None:
        self.source_name = source_name
        self.tokens = tokenize(source, source_name)
        self.position = 0
        #: Whether ``Tag{...}`` may start here. It may not where a `{` would
        #: open a block instead -- an `if` condition, a `for`'s source -- for
        #: the reason Go forbids the same thing in the same places: `if x.y {`
        #: has to keep meaning what it looks like. Brackets of any kind turn it
        #: back on, so `if (x as T{a: 1}).ok { }` is still available.
        self.brace_literals = True

    # -- token helpers ---------------------------------------------------------

    @property
    def current(self) -> Token:
        return self.tokens[self.position]

    def peek(self, offset: int = 1) -> Token:
        index = min(self.position + offset, len(self.tokens) - 1)
        return self.tokens[index]

    def advance(self) -> Token:
        token = self.tokens[self.position]
        if token.kind != "end":
            self.position += 1
        return token

    def at(self, kind: str) -> bool:
        return self.current.kind == kind

    def keyword(self, offset: int = 0) -> str:
        """The token at ``offset`` as a keyword, or ``""`` if it is not a word.

        A word written in one case throughout reads as its lower-case self, so
        ``FOR`` and ``for`` are the same keyword and ``For`` is a name.
        """
        token = self.peek(offset) if offset else self.current
        return canonical(token.value) if token.kind == "word" else ""

    def at_word(self, *words: str) -> bool:
        return self.keyword() in words if self.current.kind == "word" else False

    def skip_newlines(self) -> None:
        """Step over line breaks, where a line break cannot end a statement."""
        while self.current.kind == "newline":
            self.position += 1

    def continues_with(self, *kinds: str) -> bool:
        """Whether the next real token is one of ``kinds``, past line breaks.

        This is what lets a long pipeline wrap: a line ending just before `|` or
        `->` is a continuation, not the end of the statement.
        """
        offset = 0
        while self.peek(offset).kind == "newline":
            offset += 1
        return self.peek(offset).kind in kinds

    def end_statement(self) -> None:
        """Require the end of a statement: a line break, a '}', or the file."""
        if self.current.kind == "newline":
            self.skip_newlines()
            return
        if self.current.kind in ("}", "end"):
            return
        raise self.error(
            f"Unexpected {self.current.text!r} after a complete statement; "
            "one statement per line."
        )

    def accept(self, kind: str) -> Token | None:
        if self.current.kind == kind:
            return self.advance()
        return None

    def accept_word(self, *words: str) -> Token | None:
        if self.at_word(*words):
            return self.advance()
        return None

    def found(self) -> str:
        """What is actually here, for an error message."""
        return repr(self.current.text or "end of file")

    def expect(self, kind: str, what: str = "") -> Token:
        if self.current.kind != kind:
            raise self.error(f"Expected {what or kind}, found {self.found()}.")
        return self.advance()

    def expect_word(self, *words: str) -> Token:
        if not self.at_word(*words):
            expected = " or ".join(repr(word) for word in words)
            raise self.error(f"Expected {expected}, found {self.found()}.")
        return self.advance()

    def expect_name(self, what: str = "a name") -> Token:
        if self.current.kind != "word":
            raise self.error(f"Expected {what}, found {self.found()}.")
        return self.advance()

    def error(
        self, message: str, token: Token | None = None
    ) -> FlowSyntaxError:
        located = token or self.current
        return FlowSyntaxError(
            message, located.line, located.column, self.source_name
        )

    # -- program ---------------------------------------------------------------

    def parse_program(self) -> list[syntax.FlowDeclaration]:
        """Parse every ``flow`` declaration in the file."""
        flows: list[syntax.FlowDeclaration] = []
        self.skip_newlines()
        while not self.at("end"):
            self.expect_word("flow")
            flows.append(self.parse_flow())
            self.skip_newlines()
        if not flows:
            raise self.error("A flow file must declare at least one flow.")
        return flows

    def parse_flow(self) -> syntax.FlowDeclaration:
        token = self.tokens[self.position - 1]
        name = self.parse_dotted_name("a flow name")
        declaration = syntax.FlowDeclaration(token=token, name=name)
        self.expect("{")
        self.skip_newlines()
        while not self.at("}"):
            if self.at("end"):
                raise self.error(f"Flow {name!r} is missing its closing '}}'.")
            if self.at_word("describe") and self.peek().kind == "string":
                self.advance()
                declaration.description = self.advance().value
            elif self.at_word("in", "out") and self.peek().kind == "word":
                declaration.ports.append(self.parse_port())
            elif self.at_word("header") and self.peek().kind == "string":
                declaration.headers.append(self.parse_header())
            else:
                declaration.body.append(self.parse_statement())
            self.end_statement()
        self.expect("}")
        return declaration

    def parse_port(self) -> syntax.PortDeclaration:
        token = self.expect_word("in", "out")
        direction = "inputs" if canonical(token.value) == "in" else "outputs"
        name = self.expect_name("a port name").value
        self.expect(":")
        type_expression = self.parse_type()
        if (
            not type_expression.quoted
            and not type_expression.parameters
            and canonical(type_expression.name) in ("stream", "required")
        ):
            # These used to come first. Say so, rather than report the type
            # after them as a statement that has no business being here.
            raise self.error(
                f"{type_expression.name!r} follows the type: write "
                f"'{name}: TYPE {type_expression.name}'.",
                type_expression.token,
            )
        # `stream` and `required` are what the port is like, not what its type
        # is, so they follow the type and may be written in either order. A port
        # carries one value unless it says otherwise, because most do.
        unary = True
        required = False
        while True:
            if self.accept_word("stream") is not None:
                unary = False
            elif self.accept_word("required") is not None:
                required = True
            else:
                break
        description = self.advance().value if self.at("string") else ""
        return syntax.PortDeclaration(
            token=token,
            name=name,
            direction=direction,
            type=type_expression,
            unary=unary,
            required=required,
            description=description,
        )

    def parse_type(self) -> syntax.TypeExpression:
        """A port's type: a name, a name with type parameters, or a string.

        The name may be dotted, which is how a type registered in a
        serialisation registry is written -- ``a11.sdk.AudioBuffer`` -- and the
        brackets are how a generic one says what it holds:
        ``list[a11.NodeFragment]``. A quoted name is a mimetype.
        """
        token = self.current
        if self.at("string"):
            self.advance()
            return syntax.TypeExpression(
                token=token, name=token.value, quoted=True
            )
        name = self.parse_dotted_name("a port type")
        parameters: list[syntax.TypeExpression] = []
        if self.accept("[") is not None:
            while True:
                parameters.append(self.parse_type())
                if self.accept(",") is None:
                    break
            self.expect("]", "']' after the type parameters")
        return syntax.TypeExpression(
            token=token, name=name, parameters=parameters
        )

    def parse_header(self) -> syntax.HeaderDeclaration:
        token = self.expect_word("header")
        name = self.expect("string", "a header name").value
        alias = name.replace("-", "_").replace(".", "_")
        if self.accept_word("as") is not None:
            alias = self.expect_name("a header alias").value
        default = None
        if self.accept_word("default") is not None:
            default = self.parse_literal_value()
        description = self.advance().value if self.at("string") else ""
        return syntax.HeaderDeclaration(
            token=token,
            name=name,
            alias=alias,
            default=default,
            description=description,
        )

    def parse_literal_value(self) -> Any:
        node = self.parse_expression()
        value = _constant_value(node)
        if value is _NOT_CONSTANT:
            raise self.error("Expected a constant value.", node.token)
        return value

    # -- statements ------------------------------------------------------------

    def parse_block(self) -> list[syntax.Node]:
        self.expect("{")
        self.skip_newlines()
        body: list[syntax.Node] = []
        while not self.at("}"):
            if self.at("end"):
                raise self.error("Missing '}'.")
            body.append(self.parse_statement())
            self.end_statement()
        self.expect("}")
        return body

    def _opens_statement(self, word: str) -> bool:
        """Whether a statement-opening word is used as a keyword here."""
        if word not in STATEMENT_WORDS:
            return False
        # `skip -> out` and `wait | count -> n` treat the word as a name.
        return self.peek().kind not in ("->", "|", "=", "<-", ".", "[")

    def parse_statement(self) -> syntax.Node:
        token = self.current
        if token.kind == "word" and self._opens_statement(self.keyword()):
            word = self.keyword()
            if word in ("run", "call", "try"):
                return syntax.CallStatement(token=token, call=self.parse_call())
            if word == "skip":
                self.advance()
                return self.parse_skip(token)
            if word == "wait":
                self.advance()
                subject = self.parse_reference()
                timeout: timing.Duration | None = None
                if self.accept_word("timeout") is not None:
                    timeout = self.expect("duration", "a duration").value
                return syntax.Wait(
                    token=token,
                    subject=subject,
                    timeout=timeout,
                    after=self.parse_after(),
                )
            if word == "drain":
                self.advance()
                target = self.parse_reference()
                return syntax.Drain(
                    token=token, target=target, after=self.parse_after()
                )
            if word == "cancel":
                self.advance()
                return syntax.Cancel(
                    token=token,
                    name=self.expect_name("a call name").value,
                    after=self.parse_after(),
                )
            if word == "fail":
                self.advance()
                return self.parse_fail(token)
            if word == "for":
                return self.parse_for()
            if word == "repeat":
                return self.parse_repeat()
            if word in ("until", "while"):
                self.advance()
                return syntax.Until(
                    token=token,
                    condition=self.parse_expression(),
                    stop_when=word == "until",
                )
            if word == "if":
                return self.parse_if()
            if word == "nodes":
                self.advance()
                name = self.expect_name("a node map name").value
                body: list[syntax.Node] = []
                if self.at("{"):
                    body = self.parse_block()
                return syntax.Nodes(token=token, name=name, body=body)

        if token.kind == "word" and self.peek().kind == "=":
            name = self.advance().value
            self.advance()
            if self.at_word("node"):
                return syntax.Bind(
                    token=token, name=name, value=self.parse_node()
                )
            if self.at_word("wait", "drain"):
                return syntax.Bind(
                    token=token, name=name, value=self.parse_statement()
                )
            return syntax.Bind(
                token=token, name=name, value=self.parse_call()
            )

        if token.kind == "word" and self.peek().kind == "<-":
            name = self.advance().value
            self.advance()
            return syntax.Carry(
                token=token, name=name, pipeline=self.parse_pipeline()
            )

        pipeline = self.parse_pipeline()
        if self.continues_with("->"):
            self.skip_newlines()
        self.expect("->", "'->' and a destination port")
        targets = [self.parse_reference()]
        while self.accept(",") is not None:
            self.skip_newlines()
            targets.append(self.parse_reference())
        return syntax.Pipe(
            token=token,
            pipeline=pipeline,
            targets=targets,
            after=self.parse_after(),
        )

    def _next_word_is(self, word: str) -> bool:
        offset = 0
        while self.peek(offset).kind == "newline":
            offset += 1
        token = self.peek(offset)
        return token.kind == "word" and token.value == word

    def parse_skip(self, token: Token) -> syntax.Skip:
        """``skip pipeline``, or ``skip n reference`` for the first ``n``.

        The counted form takes a reference rather than a pipeline because the
        count belongs to the node: it is the node's first ``n`` values that go
        unread, for every reader of it, which is not something a pipeline of
        one reader's own could say.
        """
        count: int | None = None
        if self.at("number"):
            number = self.advance()
            if not isinstance(number.value, int) or number.value < 1:
                raise self.error(
                    f"'skip' counts whole values, so {number.text} is not a "
                    f"number of them to skip.",
                    number,
                )
            count = number.value
            source = syntax.Pipeline(
                token=self.current, source=self.parse_reference()
            )
        else:
            source = self.parse_pipeline()
        return syntax.Skip(
            token=token,
            pipeline=source,
            after=self.parse_after(),
            count=count,
        )

    def parse_fail(self, token: Token) -> syntax.Fail:
        """``fail``, ``fail thing``, or ``fail code thing``."""
        code: syntax.Node | None = None
        message: syntax.Node | None = None
        if not self._at_statement_end():
            code = self.parse_expression()
        if not self._at_statement_end():
            message = self.parse_expression()
        if message is None and code is not None:
            # A lone operand is a message when it looks like one, and a status
            # or a code when it does not; the runtime decides on the value.
            pass
        return syntax.Fail(
            token=token,
            code=code,
            message=message,
            after=self.parse_after(),
        )

    def parse_node(self) -> syntax.NodeExpression:
        """``node()``, ``node(id)``, or either of those with ``in <map>``.

        The parentheses are not decoration: making a node is the one thing in
        the language that *does* something without naming an action, and
        `x = node()` reads as the construction it is. Bare `node` would also
        keep the word from ever being anything else, which a language with no
        reserved words has no business doing.
        """
        token = self.expect_word("node")
        if not self.at("("):
            raise self.error(
                "Making a node takes parentheses: 'node()', or 'node(id)' to "
                "attach to one somebody else named.",
                token,
            )
        self.expect("(")
        node_id: syntax.Node | None = None
        if not self.at(")"):
            node_id = self.parse_expression()
        self.expect(")")
        node_map: str | None = None
        if self.accept_word("in") is not None:
            node_map = self.expect_name("a node map name").value
        return syntax.NodeExpression(
            token=token, id=node_id, node_map=node_map
        )

    def _at_statement_end(self) -> bool:
        return self.current.kind in ("newline", "}", "end") or self.at_word(
            "after"
        )

    def parse_after(self) -> list[str]:
        """A trailing ``after a, b`` on a statement that is not a call."""
        names: list[str] = []
        if self.accept_word("after") is None:
            return names
        names.append(self.expect_name("a step name").value)
        while self.at(",") and self.peek().kind == "word":
            self.advance()
            names.append(self.expect_name("a step name").value)
        return names

    def parse_for(self) -> syntax.ForEach:
        token = self.expect_word("for")
        variable = self.expect_name("a loop variable").value
        self.expect_word("in")
        pipeline = self.parse_block_header(self.parse_pipeline)
        parallel = 1
        if self.accept_word("parallel") is not None:
            parallel = int(self.expect("number", "a count").value)
        return syntax.ForEach(
            token=token,
            variable=variable,
            pipeline=pipeline,
            parallel=parallel,
            body=self.parse_block(),
        )

    def parse_repeat(self) -> syntax.Repeat:
        token = self.expect_word("repeat")
        variable: str | None = None
        start: syntax.Node | None = None
        if self.current.kind == "word" and self.peek().kind == "=":
            variable = self.advance().value
            self.advance()
            start = self.parse_block_header(self.parse_expression)
        max_iterations = 16
        if self.accept_word("max") is not None:
            max_iterations = int(self.expect("number", "a count").value)
        return syntax.Repeat(
            token=token,
            variable=variable,
            start=start,
            max_iterations=max_iterations,
            body=self.parse_block(),
        )

    def parse_if(self) -> syntax.If:
        token = self.expect_word("if")
        condition = self.parse_block_header(self.parse_expression)
        then_body = self.parse_block()
        else_body: list[syntax.Node] = []
        if self.continues_with("word") and self._next_word_is("else"):
            self.skip_newlines()
        if self.accept_word("else") is not None:
            if self.at_word("if"):
                else_body = [self.parse_if()]
            else:
                else_body = self.parse_block()
        return syntax.If(
            token=token,
            condition=condition,
            then_body=then_body,
            else_body=else_body,
        )

    # -- calls -----------------------------------------------------------------

    def parse_call(self) -> syntax.CallExpression:
        token = self.current
        tolerant = self.accept_word("try") is not None
        # The verb is the dispatch: `run` binds the handler registered here,
        # `call` puts the action on the stream this flow is attached to. A11
        # itself draws the line in the same place, between `Action::Run` and
        # `Action::Call`, so a flow says it the way everything else does.
        verb = self.expect_word("run", "call")
        mode = canonical(verb.value)
        action = self.parse_dotted_name("an action name")
        args: list[tuple[str, syntax.Pipeline]] = []
        self.expect("(")
        self.skip_newlines()
        while not self.at(")"):
            name = self.expect_name("a port name").value
            self.expect(":")
            args.append((name, self.parse_pipeline()))
            self.skip_newlines()
            if self.accept(",") is None:
                break
            self.skip_newlines()
        self.expect(")")
        return syntax.CallExpression(
            token=token,
            action=action,
            mode=mode,
            args=args,
            modifiers=self.parse_modifiers(),
            tolerant=tolerant,
        )

    def _continues_with_modifier(self) -> bool:
        """Whether a line break is followed by a modifier for this call.

        Modifiers read well on a line of their own, so a break before one
        continues the call -- unless what follows looks like a statement in its
        own right, which is what a port called ``timeout`` left of a `->` is.
        """
        offset = 0
        while self.peek(offset).kind == "newline":
            offset += 1
        if offset == 0:
            return False
        if self.keyword(offset) not in MODIFIER_WORDS:
            return False
        return self.peek(offset + 1).kind not in ("->", "|", "=", "<-")

    def parse_modifiers(self) -> syntax.CallModifiers:
        modifiers = syntax.CallModifiers(token=self.current)
        while True:
            if self._continues_with_modifier():
                self.skip_newlines()
            if self.keyword() not in MODIFIER_WORDS:
                break
            word = self.advance()
            modifier = canonical(word.value)
            if modifier == "tee":
                modifiers.tee = True
            elif modifier == "via":
                modifiers.node_map = self.expect_name("a node map name").value
            elif modifier == "timeout":
                modifiers.timeout = self.expect("duration", "a duration").value
            elif modifier == "id":
                modifiers.action_id = self.parse_expression()
            elif modifier == "after":
                modifiers.after.append(self.expect_name("a call name").value)
                while self.at(",") and self.peek().kind == "word":
                    self.advance()
                    modifiers.after.append(
                        self.expect_name("a call name").value
                    )
            elif modifier == "forward":
                # `forward headers "a", "b"`: send the call the headers this
                # flow was given, without naming a value for each.
                self.expect_word("headers")
                while True:
                    modifiers.forward.append(
                        self.expect("string", "a header name").value
                    )
                    if self.at(",") and self.peek().kind == "string":
                        self.advance()
                        continue
                    break
            elif modifier == "with":
                while True:
                    header = self.expect("string", "a header name").value
                    self.expect(":")
                    modifiers.headers.append((header, self.parse_expression()))
                    if self.at(",") and self.peek().kind == "string":
                        self.advance()
                        continue
                    break
            else:  # `headers`, reached without the `forward` that owns it.
                raise self.error(
                    f"{word.text!r} belongs to 'forward headers'; write "
                    "'forward headers \"x-name\"'.",
                    word,
                )
        return modifiers

    # -- pipelines -------------------------------------------------------------

    def parse_pipeline(self) -> syntax.Pipeline:
        token = self.current
        source = self.parse_expression()
        stages: list[syntax.Stage] = []
        while True:
            # `then` and `where` read as words between the things they join --
            # `history then asked`, `hits where it.ok` -- so the pipe is
            # optional in front of those two. Everything else is a
            # transformation applied to a stream, which is what `|` says, and
            # keeping the bar there is what stops a stage name from swallowing
            # the port that happens to share its name.
            if self.continues_with("|") or self._continues_with_bare_stage():
                self.skip_newlines()
            if self.accept("|") is not None:
                self.skip_newlines()
                stages.append(self.parse_stage())
                continue
            if self._at_bare_stage():
                stages.append(self.parse_stage())
                continue
            break
        return syntax.Pipeline(token=token, source=source, stages=stages)

    def _at_bare_stage(self) -> bool:
        """Whether a stage that may go without its pipe starts here."""
        if self.keyword() not in BARE_STAGES:
            return False
        # `then` and `where` take an operand, so a bare one at the end of a
        # statement is a name that happens to be spelled like a stage.
        return self.peek().kind not in ("newline", "end", "}", "->", ",")

    def _continues_with_bare_stage(self) -> bool:
        """Whether a line break is followed by a bare `then`/`where`."""
        offset = 0
        while self.peek(offset).kind == "newline":
            offset += 1
        if offset == 0:
            return False
        if self.keyword(offset) not in BARE_STAGES:
            return False
        return self.peek(offset + 1).kind not in ("->", "|", "=", "<-")

    def parse_stage(self) -> syntax.Stage:
        token = self.expect_name("a stage name")
        name = canonical(token.value)
        if name not in STAGES:
            known = ", ".join(sorted(STAGES))
            raise self.error(f"Unknown stage {name!r} (known: {known}).", token)
        takes = STAGES[name]
        argument: Any = None
        if takes == "number":
            argument = self.expect("number", f"a count for '{name}'").value
        elif takes == "string":
            argument = self.expect("string", f"a pattern for '{name}'").value
        elif takes == "string?":
            argument = self.advance().value if self.at("string") else ""
        elif takes == "expr":
            argument = self.parse_expression()
        elif takes == "stream":
            # A stream rather than a value: `then` reads this one and then
            # that one, so its argument is whatever a pipeline may start with.
            argument = self.parse_postfix()
        return syntax.Stage(token=token, name=name, arg=argument)

    # -- expressions -----------------------------------------------------------

    def parse_reference(self) -> syntax.Node:
        """Parse a name, optionally with ``.port`` -- what a pipe writes to and
        what `wait`, `drain`, `status` and a counted `skip` take."""
        node = self.parse_postfix()
        if not isinstance(node, (syntax.Name, syntax.Attr, syntax.Outcome)):
            raise self.error(
                "Expected a port or a node here, like 'out-port' or "
                "'call.port'.",
                node.token,
            )
        return node

    def parse_expression(self) -> syntax.Node:
        return self.parse_or()

    def parse_or(self) -> syntax.Node:
        left = self.parse_and()
        while self.at_word("or"):
            token = self.advance()
            left = syntax.Binary(
                token=token, op="or", left=left, right=self.parse_and()
            )
        return left

    def parse_and(self) -> syntax.Node:
        left = self.parse_not()
        while self.at_word("and"):
            token = self.advance()
            left = syntax.Binary(
                token=token, op="and", left=left, right=self.parse_not()
            )
        return left

    def parse_not(self) -> syntax.Node:
        if self.at_word("not"):
            token = self.advance()
            return syntax.Unary(token=token, op="not", operand=self.parse_not())
        return self.parse_comparison()

    def parse_comparison(self) -> syntax.Node:
        left = self.parse_additive()
        if self.current.kind in _COMPARISONS:
            token = self.advance()
            return syntax.Binary(
                token=token,
                op=token.kind,
                left=left,
                right=self.parse_additive(),
            )
        if self.at_word("in"):
            token = self.advance()
            return syntax.Binary(
                token=token, op="in", left=left, right=self.parse_additive()
            )
        return left

    def parse_additive(self) -> syntax.Node:
        """``a + b`` and ``a - b``: numbers, and times.

        The only arithmetic the language has, and it is here for durations --
        "how long did that take", "is this older than the deadline" -- which a
        composition cannot express any other way. `-` needs its spaces: `a-b`
        is one name, because an action is called `text-upper`.
        """
        left = self.parse_cast()
        while self.current.kind in ("+", "-"):
            token = self.advance()
            left = syntax.Binary(
                token=token,
                op=token.kind,
                left=left,
                right=self.parse_cast(),
            )
        return left

    def parse_cast(self) -> syntax.Node:
        """``expr as TYPE`` -- the value, made a value of that type."""
        node = self.parse_postfix()
        while self.at_word("as"):
            token = self.advance()
            node = syntax.TypedValue(
                token=token, type=self.parse_type(), value=node
            )
        return node

    def parse_block_header(self, parse) -> Any:
        """Parse ``parse()`` where a '{' after it opens a block, not a value."""
        outer = self.brace_literals
        self.brace_literals = False
        try:
            return parse()
        finally:
            self.brace_literals = outer

    def parse_postfix(self) -> syntax.Node:
        node = self.parse_primary()
        while True:
            if self.at("."):
                token = self.advance()
                name = self.expect_name("a name after '.'").value
                node = syntax.Attr(token=token, base=node, name=name)
                continue
            if self.at("["):
                token = self.advance()
                index = self.parse_expression()
                self.expect("]")
                node = syntax.Index(token=token, base=node, index=index)
                continue
            # `a11.sdk.Interaction{...}`: a value of a named type, written the
            # way the type's own fields read. A generic one is spelled with
            # `as`, where the brackets cannot be mistaken for an index.
            if self.at("{") and self.brace_literals:
                name = _dotted_name(node)
                if name is None:
                    return node
                node = syntax.TypedValue(
                    token=node.token,
                    type=syntax.TypeExpression(token=node.token, name=name),
                    value=self.parse_object_literal(),
                )
                continue
            return node

    def parse_primary(self) -> syntax.Node:
        token = self.current
        # `status` reads an outcome when something follows it to be the outcome
        # *of*; on its own it is an ordinary name, so a port may be called that.
        if self.at_word("status") and self.peek().kind == "word":
            self.advance()
            return syntax.Outcome(
                token=token, subject=self.parse_reference()
            )
        if token.kind == "string":
            self.advance()
            return syntax.Literal(token=token, value=token.value)
        if token.kind in ("number", "duration"):
            self.advance()
            return syntax.Literal(token=token, value=token.value)
        if token.kind == "(":
            self.advance()
            # Inside brackets a '{' cannot be opening a block, so a typed
            # value is available again however this expression was reached.
            outer, self.brace_literals = self.brace_literals, True
            try:
                self.skip_newlines()
                node = self.parse_expression()
                if self.at("|"):
                    stages: list[syntax.Stage] = []
                    while self.accept("|") is not None:
                        self.skip_newlines()
                        stages.append(self.parse_stage())
                    self.skip_newlines()
                    self.expect(")")
                    return syntax.PipelineValue(
                        token=token,
                        pipeline=syntax.Pipeline(
                            token=token, source=node, stages=stages
                        ),
                    )
                self.skip_newlines()
                self.expect(")")
                return node
            finally:
                self.brace_literals = outer
        if token.kind == "[":
            self.advance()
            outer, self.brace_literals = self.brace_literals, True
            try:
                self.skip_newlines()
                items: list[syntax.Node] = []
                while not self.at("]"):
                    items.append(self.parse_expression())
                    self.skip_newlines()
                    if self.accept(",") is None:
                        break
                    self.skip_newlines()
                self.expect("]")
                return syntax.ListLiteral(token=token, items=items)
            finally:
                self.brace_literals = outer
        if token.kind == "{":
            return self.parse_object_literal()
        if token.kind == "word":
            spelled = self.advance().value
            word = canonical(spelled)
            if word == "true":
                return syntax.Literal(token=token, value=True)
            if word == "false":
                return syntax.Literal(token=token, value=False)
            if word == "null":
                return syntax.Literal(token=token, value=None)
            if word == "it":
                return syntax.It(token=token)
            if self.at("(") and word in BUILTINS:
                self.advance()
                self.skip_newlines()
                args: list[syntax.Node] = []
                while not self.at(")"):
                    args.append(self.parse_expression())
                    self.skip_newlines()
                    if self.accept(",") is None:
                        break
                    self.skip_newlines()
                self.expect(")")
                return syntax.Builtin(token=token, name=word, args=args)
            if self.at("("):
                raise self.error(
                    f"{spelled!r} is not a built-in function "
                    f"(known: {', '.join(sorted(BUILTINS))}).",
                    token,
                )
            return syntax.Name(token=token, name=spelled)
        raise self.error(
            f"Expected a value, found {token.text or 'end of file'!r}.", token
        )

    def parse_object_literal(self) -> syntax.ObjectLiteral:
        """``{ key: expr, ... }``, wherever one is allowed.

        Inside the braces a line break is never the end of anything, so an
        object with a field per line reads the way it is written.
        """
        token = self.expect("{")
        outer, self.brace_literals = self.brace_literals, True
        self.skip_newlines()
        pairs: list[tuple[str, syntax.Node]] = []
        while not self.at("}"):
            if self.at("string"):
                key = self.advance().value
            else:
                key = self.expect_name("an object key").value
            self.expect(":")
            pairs.append((key, self.parse_expression()))
            self.skip_newlines()
            if self.accept(",") is None:
                break
            self.skip_newlines()
        self.expect("}")
        self.brace_literals = outer
        return syntax.ObjectLiteral(token=token, pairs=pairs)

    def parse_dotted_name(self, what: str) -> str:
        if self.at("string"):
            return self.advance().value
        parts = [self.expect_name(what).value]
        while self.at(".") and self.peek().kind == "word":
            self.advance()
            parts.append(self.advance().value)
        return ".".join(parts)


def _dotted_name(node: syntax.Node) -> str | None:
    """``a11.sdk.AudioBuffer`` for a chain of plain names, else ``None``.

    A tag is the only thing on the left of a ``{`` that means a type, and it
    arrives as the same `Name`/`Attr` chain any other dotted reference does.
    """
    parts: list[str] = []
    while isinstance(node, syntax.Attr):
        parts.append(node.name)
        node = node.base
    if not isinstance(node, syntax.Name):
        return None
    parts.append(node.name)
    return ".".join(reversed(parts))


class _NotConstant:
    pass


_NOT_CONSTANT = _NotConstant()


def _constant_value(node: syntax.Node) -> Any:
    if isinstance(node, syntax.Literal):
        return node.value
    if isinstance(node, syntax.ListLiteral):
        values = [_constant_value(item) for item in node.items]
        return (
            _NOT_CONSTANT
            if any(value is _NOT_CONSTANT for value in values)
            else values
        )
    if isinstance(node, syntax.ObjectLiteral):
        result = {}
        for key, item in node.pairs:
            value = _constant_value(item)
            if value is _NOT_CONSTANT:
                return _NOT_CONSTANT
            result[key] = value
        return result
    return _NOT_CONSTANT


def constant_value(node: syntax.Node) -> tuple[bool, Any]:
    """``(True, value)`` when ``node`` is a literal all the way down."""
    value = _constant_value(node)
    return (False, None) if value is _NOT_CONSTANT else (True, value)


def parse(source: str, source_name: str = "") -> list[syntax.FlowDeclaration]:
    """Parse Flow source into its flow declarations.

    Raises:
        FlowSyntaxError: With the line and column of the first problem.
    """
    return Parser(source, source_name).parse_program()


__all__ = [
    "BARE_STAGES",
    "BUILTINS",
    "DECLARATION_WORDS",
    "MODIFIER_WORDS",
    "SOURCE_WORDS",
    "STAGES",
    "STATEMENT_WORDS",
    "Parser",
    "constant_value",
    "parse",
]
