"""Running a `.flow` file as a program, from this process.

`a11.flow.run_program` is the same interpreter `a11-flow-run` is, so almost
everything about a program is already covered by the C++ tests and by
`examples/006-flow-programs`. What is *only* true here is the reason the binding
exists: this process has actions of its own, and a program can call them.

So these tests are about the seam rather than the language. The two halves of it
that can break independently:

* the **policy** is what the caller passed and nothing the file said, so a
  refusal has to arrive as a refusal rather than as an empty result;
* a **host-registered** action runs on the caller's Python, which means the GIL,
  a loop that is not this thread, and a handler whose failure has to become the
  program's failure.

!!! note "Why every call is in a thread"

    `run_program` blocks until the program finishes. An `async def` handler
    needs a loop to drive it, and if that loop is on *this* thread it cannot
    run while the call is blocking it -- the program then waits forever on its
    own handler.
    `asyncio.to_thread` is the documented pattern and is used even for the
    programs that need no handler, so no test here can be copied into one that
    hangs.
"""

from __future__ import annotations

import asyncio
import pathlib

import pytest

import a11
from a11 import flow
from a11.actions import ActionRegistry

# The flow in `ASKS_A_MODEL` names `a11.sdk.Interaction` in a `map` tag, and a
# tag resolves only to a type this process has imported.
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import Interaction

PATIENCE = 30


async def run(source: str, **how) -> dict:
    """`run_program` off the loop, with a bound on how long it may take.

    The timeout is the point: every failure mode this file is testing -- a
    handler that is never scheduled, a program parked on a stream nothing will
    close -- presents as a hang, and a hanging test is a test nobody reads.
    """
    return await asyncio.wait_for(
        asyncio.to_thread(flow.run_program, source, "test.flow", **how),
        timeout=PATIENCE,
    )


# --- the plainest thing a program does ---------------------------------------


ARGV_TO_STDOUT = """
flow {
  describe "Write what was passed on the command line."

  nodes scratch
  who = node() in scratch

  argv | drop 1 then "world" | first 1 -> who

  out = run write_stdout(
    content: who | map strformat("Hello, %s!\\n", trim(it))
  ) via scratch
  skip out.bytes_written
}
"""


@pytest.mark.asyncio
async def test_argv_reaches_stdout(capfd):
    outcome = await run(ARGV_TO_STDOUT, arguments=["test.flow", "Helena"])
    assert outcome["exit_code"] == 0
    # capfd, not capsys: the bytes are written by `write_stdout` through the
    # process's file descriptor 1, so nothing Python-level ever sees them.
    assert capfd.readouterr().out == "Hello, Helena!\n"


@pytest.mark.asyncio
async def test_argv_falls_through_to_its_default(capfd):
    outcome = await run(ARGV_TO_STDOUT, arguments=["test.flow"])
    assert outcome["exit_code"] == 0
    assert capfd.readouterr().out == "Hello, world!\n"


# --- an action this process registered ---------------------------------------


SHOUT = a11.ActionSchema(
    name="shout",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", typeinfo=str, required=True
        )
    },
    outputs={
        "result": a11.ActionPortSchema(
            name="result", type="text/plain", typeinfo=str, required=True
        )
    },
)


CALLS_SHOUT = """
flow {
  describe "Put an argument through an action the host registered."

  nodes scratch
  said = node() in scratch

  argv | drop 1 | first 1 -> said

  loud = run shout(text: said) via scratch
  out = run write_stdout(content: loud.result | map strformat("%s\\n", it))
    via scratch
  skip out.bytes_written
}
"""


@pytest.mark.asyncio
async def test_a_flow_calls_a_python_action(capfd):
    async def shout(action):
        text = await action["text"].consume()
        await action["result"].finalize(text.upper())

    registry = ActionRegistry()
    registry.register("shout", SHOUT, shout)

    outcome = await run(
        CALLS_SHOUT, arguments=["test.flow", "quietly"], registry=registry
    )
    assert outcome["exit_code"] == 0
    assert capfd.readouterr().out == "QUIETLY\n"


@pytest.mark.asyncio
async def test_a_failing_python_action_fails_the_program():
    """A handler's error is the program's error, not a silent empty port.

    Worth its own test because the failure has to cross the boundary twice: a
    Python exception becomes an action status on a fibre with no GIL, and that
    status has to come back out of `run_program` as an exception again.
    """

    async def refuse(action):
        raise RuntimeError("not in the mood")

    registry = ActionRegistry()
    registry.register("shout", SHOUT, refuse)

    with pytest.raises(Exception, match="not in the mood"):
        await run(
            CALLS_SHOUT, arguments=["test.flow", "quietly"], registry=registry
        )


@pytest.mark.asyncio
async def test_the_host_keeps_its_own_action_of_a_standard_name(tmp_path):
    """A name already registered is never replaced by the standard library's.

    This is the property that makes the registry argument safe to pass: a host
    that registered its own `read_file` meant its own, and a program it runs
    must not silently get the real filesystem instead.
    """
    called = asyncio.Event()

    async def fake_read_file(action):
        called.set()
        await action["content"].finalize("not from any disk")

    registry = ActionRegistry()
    registry.register(
        "read_file",
        a11.ActionSchema(
            name="read_file",
            inputs={
                "path": a11.ActionPortSchema(
                    name="path", type="text/plain", typeinfo=str, required=True
                )
            },
            outputs={
                "content": a11.ActionPortSchema(
                    name="content",
                    type="text/plain",
                    typeinfo=str,
                    required=True,
                )
            },
        ),
        fake_read_file,
    )

    real = tmp_path / "on-disk.txt"
    real.write_text("from the disk")
    source = """
flow {
  nodes scratch
  where = node() in scratch
  argv | drop 1 | first 1 -> where
  got = run read_file(path: where) via scratch
  out = run write_stdout(content: got.content) via scratch
  skip out.bytes_written
}
"""
    outcome = await run(
        source,
        arguments=["test.flow", str(real)],
        roots=[str(tmp_path)],
        registry=registry,
    )
    assert outcome["exit_code"] == 0
    assert called.is_set()


# --- the policy is the caller's ----------------------------------------------


READS_A_PATH = """
flow {
  nodes scratch
  where = node() in scratch
  argv | drop 1 | first 1 -> where
  got = run read_file(path: where) via scratch
  skip got.info
  skip got.lines
  skip got.bytes
  out = run write_stdout(content: got.text) via scratch
  skip out.bytes_written
}
"""


@pytest.mark.asyncio
async def test_a_read_outside_the_roots_is_refused(tmp_path):
    outside = tmp_path.parent / f"{tmp_path.name}-elsewhere.txt"
    outside.write_text("not yours")
    inside = tmp_path / "yours.txt"
    inside.write_text("yours")
    try:
        # The same program, the same interpreter: only the root differs, so a
        # pass here would mean the root was never consulted at all.
        outcome = await run(
            READS_A_PATH,
            arguments=["test.flow", str(inside)],
            roots=[str(tmp_path)],
        )
        assert outcome["exit_code"] == 0

        with pytest.raises(Exception) as refused:
            await run(
                READS_A_PATH,
                arguments=["test.flow", str(outside)],
                roots=[str(tmp_path)],
            )
        # The message has to name the path, or a policy refusal is
        # indistinguishable from the file not being there.
        assert str(outside) in str(refused.value) or "outside" in str(
            refused.value
        )
    finally:
        outside.unlink(missing_ok=True)


@pytest.mark.asyncio
async def test_writing_is_refused_unless_it_was_allowed(tmp_path):
    """`allow_write=False` does not register a writing action at all.

    So the refusal arrives while the program is being resolved rather than while
    it runs -- which is the better place for it, and is why this asserts on the
    failure and not on the file being absent.
    """
    source = """
flow {
  nodes scratch
  out = run write_file(path: "%s", content: "nope") via scratch
  skip out.bytes_written
}
""" % (tmp_path / "should-not-appear.txt")

    with pytest.raises(Exception):
        await run(source, roots=[str(tmp_path)])
    assert not (tmp_path / "should-not-appear.txt").exists()

    outcome = await run(source, roots=[str(tmp_path)], allow_write=True)
    assert outcome["exit_code"] == 0
    assert (tmp_path / "should-not-appear.txt").read_text() == "nope"


# --- the exit code ------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_program_chooses_its_exit_code():
    source = """
flow {
  describe "Fail, on purpose."
  out exit_code: integer "What the program decided."

  nodes scratch
  decided = node() in scratch
  argv | drop 1 | first 1 -> decided
  decided | map 3 -> exit_code
}
"""
    outcome = await run(source, arguments=["test.flow", "anything"])
    assert outcome["exit_code"] == 3


@pytest.mark.asyncio
async def test_a_program_that_says_nothing_exits_zero():
    outcome = await run(ARGV_TO_STDOUT, arguments=["test.flow"])
    assert outcome["exit_code"] == 0


# --- standard_streams=False ---------------------------------------------------


READS_STDIN = """
flow {
  nodes scratch
  total = node() in scratch
  input = run read_stdin() via scratch
  skip input.bytes
  skip input.is_tty
  input.lines | count -> total
  out = run write_stdout(content: total | map strformat("%d\\n", it))
    via scratch
  skip out.bytes_written
}
"""


@pytest.mark.asyncio
async def test_without_standard_streams_a_program_reading_them_fails():
    """The point of the flag: fail rather than wait forever.

    A host with no useful standard input -- a server, a notebook, anything whose
    stdin is a terminal it does not own -- wants a program that reads stdin to
    say so. The alternative is a program that hangs, which in a server is
    indistinguishable from one doing work.
    """
    with pytest.raises(Exception):
        await run(READS_STDIN, standard_streams=False)


# --- the one the CLI cannot do ------------------------------------------------


ASKS_A_MODEL = """
flow {
  describe "Put a question from the command line to a model."

  nodes scratch
  question = node() in scratch
  answer = node() in scratch

  argv | drop 1 | first 1 -> question

  llm = run interact_with_llm(
    interactions: question | map a11.sdk.Interaction{
      role: "user",
      content: [to_chunk({
        role: "user",
        content: [{type: "text", text: it}]
      })]
    },
    config: {}
  ) via scratch
  skip llm.event_stream
  skip llm.thoughts
  skip llm.new_interactions

  llm.text_output -> answer

  out = run write_stdout(content: answer | map strformat("%s\\n", it))
    via scratch
  skip out.bytes_written
}
"""


@pytest.mark.asyncio
async def test_a_flow_asks_a_model_this_process_registered(capfd):
    """A `.flow` file asking a model, with no gateway and no subprocess.

    This is the capability the command line cannot have and the reason
    `registry` is an argument at all: `interact_with_llm` needs credentials and
    an HTTP client that live in the *host*, so a program can only reach one
    when the host hands it over. The handler here is a fake -- what is being
    tested is the wiring, not a provider.
    """
    asked: list[str] = []

    async def pretend_to_think(action):
        async for interaction in action["interactions"]:
            # A real `a11.sdk.Interaction`, built by the flow's tagged literal
            # and constructed through the bridge into this interpreter -- which
            # is the half of this that only works from here.
            assert isinstance(interaction, Interaction)
            assert interaction.role.value == "user"
            for chunk in interaction.content:
                asked.append(bytes(chunk.data).decode())
        await action["text_output"].finalize("Ask me a harder one.")

    registry = ActionRegistry()
    registry.register(
        "interact_with_llm", INTERACT_WITH_LLM_SCHEMA, pretend_to_think
    )

    outcome = await run(
        ASKS_A_MODEL,
        arguments=["test.flow", "why is the sky blue"],
        registry=registry,
    )
    assert outcome["exit_code"] == 0
    assert capfd.readouterr().out == "Ask me a harder one.\n"
    # The question reached the model rather than the flow answering itself.
    assert any("why is the sky blue" in text for text in asked)


# --- check_program ------------------------------------------------------------


@pytest.mark.asyncio
async def test_check_program_describes_without_running(capfd):
    described = flow.check_program(ARGV_TO_STDOUT, "test.flow")
    assert "Write what was passed on the command line." in described
    # Nothing ran: `check` compiles and stops.
    assert capfd.readouterr().out == ""


def test_check_program_refuses_a_file_with_no_entry_flow():
    with pytest.raises(Exception):
        flow.check_program("flow named { in a: string }", "test.flow")


def test_check_program_reports_where_the_syntax_broke():
    with pytest.raises(Exception) as broken:
        flow.check_program("flow { nodes ", "test.flow")
    assert "test.flow" in str(broken.value)


# --- the examples, through this binding ---------------------------------------

EXAMPLES = pathlib.Path(__file__).parents[3] / "examples" / "006-flow-programs"


@pytest.mark.asyncio
async def test_the_example_programs_all_check():
    """Every shipped program compiles through the binding, not just the CLI.

    Cheap, and it is the test that fails when a vocabulary change lands without
    the examples being updated.
    """
    if not EXAMPLES.is_dir():
        pytest.skip("the flow program examples are not in this checkout")
    programs = sorted(EXAMPLES.glob("*.flow"))
    assert programs, "no example programs found"
    for program in programs:
        described = flow.check_program(program.read_text(), str(program))
        assert described, f"{program.name} described as nothing"
