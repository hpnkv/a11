# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Native operation expressions, module linking, and editable source APIs."""

import pytest

from a11 import flow
from a11.actions import ActionRegistry, ActionSchema
from a11.flow.diagnostics import Diagnostic, FlowSyntaxError
from a11.status import StatusCode, StatusException


@pytest.mark.asyncio
async def test_anonymous_operations_preserve_streaming_and_scalar_contexts():
    program = flow.loads("""
flow render { out text: string stream
  ["a", "b"] | flatten -> text
}
flow main {
  out stream: string stream
  out report: object
  (run render()).text -> stream
  {"text": (run render()).text} -> report
}
""")
    result = await program["main"].invoke(registry=ActionRegistry())
    assert result == {"stream": ["a", "b"], "report": {"text": "a"}}


@pytest.mark.asyncio
@pytest.mark.parametrize("items", [[], [1, 2]])
async def test_consume_checks_cardinality(items):
    program = flow.loads(
        "flow f { in values: int stream\n out result: int\n values |"
        " consume -> result }"
    )
    with pytest.raises(StatusException) as raised:
        await program.main.invoke(values=items, registry=ActionRegistry())
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION


@pytest.mark.asyncio
async def test_imported_public_flow_captures_private_dependencies():
    program = flow.loads(
        'import "library" as lib\nflow main { out result: int\n (run'
        " lib::work()).result -> result }",
        "root",
        modules={
            "library": (
                "flow hidden { out result: int\n 42 -> result }\nflow work"
                " exported { out result: int\n (run hidden()).result ->"
                " result }"
            )
        },
    )
    assert (await program.main.invoke(registry=ActionRegistry()))[
        "result"
    ] == 42
    with pytest.raises(FlowSyntaxError):
        flow.loads(
            'import "library" as lib\nflow f { run lib::hidden() }',
            modules={"library": "flow hidden {}"},
        )


def test_native_construction_and_source_edits_are_revisioned():
    document = flow.ModuleDocument("root")
    shape = flow.StructBuilder("Row")
    shape.exported = True
    shape.field("title", flow.Type("string"), required=True)
    document.insert_struct(shape)
    builder = flow.FlowBuilder("render")
    builder.exported = True
    builder.port("text", flow.Type("string"), output=True)
    builder.pipe(
        flow.Expression.literal("ready"), flow.Expression.reference("text")
    )
    document.insert(builder)
    snapshot = document.compile()
    symbol = next(
        symbol for symbol in document.symbols() if symbol.name == "render"
    )
    document.set_exported(symbol, False)
    assert snapshot["render"].exported
    assert not document.compile()["render"].exported
    with pytest.raises(StatusException):
        document.remove(symbol)


def test_declaration_upsert_preserves_comments_and_is_idempotent():
    document = flow.ModuleDocument(
        "root", "declare flow render {\n # preserved\n out text: string\n}\n"
    )
    schema = ActionSchema.model_validate(
        {"name": "render", "outputs": {"text": {"type": str, "unary": True}}}
    )
    document.upsert_declaration(schema, symbol=document.symbols()[0])
    assert "# preserved" in document.source
    again = document.upsert_declaration(schema, symbol=document.symbols()[0])
    assert not again.edits
    assert document.effective_schema("render")["outputs"]["text"]["unary"]


def test_graph_rename_retains_aliases_and_returns_reviewable_edits():
    document = flow.ModuleDocument(
        "library",
        'flow render exported { out result: string\n "ready" -> result }',
    )
    changed = document.rename_in_graph(
        document.symbols()[0],
        "draw",
        modules={
            "root": (
                'import "library" as lib\nflow main { (run'
                " lib::render()).result -> _ }"
            )
        },
    )
    assert "lib::draw" in changed.sources["root"]
    assert changed.edits["library"]
    assert "flow render" in document.source
    assert changed.diagnostics


def test_related_diagnostic_locations_and_manual_fixes_roundtrip():
    payload = {
        "code": "flow.sequence.scalar-rvalue-read",
        "message": "consume",
        "range": {},
        "source": "root",
        "related": [
            {"source": "library", "range": {}, "message": "defined here"}
        ],
        "fixes": [{"label": "Consume", "automatic": False, "edits": []}],
    }
    diagnostic = Diagnostic.from_payload(payload)
    assert diagnostic.related[0].source == "library"
    assert not diagnostic.fixes[0].automatic
    assert Diagnostic.from_payload(diagnostic.as_json()) == diagnostic


@pytest.mark.asyncio
async def test_register_all_publishes_root_implementations_without_attributes():
    registry = ActionRegistry()
    program = flow.loads(
        'import "library" as lib\ndeclare flow external {}\n'
        "struct Report { title: string }\n"
        'flow first { out report: Report\n Report{title: "ready"} -> report }\n'
        "flow public exported {}",
        modules={"library": "flow helper {}\nflow work exported {}"},
    )
    program.register_all(registry)
    assert registry.is_registered("public")
    assert registry.is_registered("first")
    assert not registry.is_registered("work")
    assert not registry.is_registered("lib.work")
    assert not registry.is_registered("external")
    result = await program["first"].invoke(registry=registry)
    assert result["report"].title == "ready"


@pytest.mark.asyncio
async def test_typed_statements_and_modifiers_emit_executable_flow():
    expression = flow.Expression
    options = flow.CallOptions()
    options.timeout = "1s"
    options.headers = {
        "z-last": expression.literal("z"),
        "a-first": expression.literal("a"),
    }
    call = expression.invoke_with("work", {}, options)
    assert call.source.index("z-last") < call.source.index("a-first")
    obj = expression.object(
        {"z-last": expression.literal(1), "a-first": expression.literal(2)}
    )
    assert obj.source.index("z-last") < obj.source.index("a-first")
    builder = flow.FlowBuilder("main")
    builder.port("out", flow.Type("string"), output=True)
    builder.add(
        flow.Statement.if_(
            expression.literal(True),
            [
                flow.Statement.pipe(
                    expression.literal("ready"), expression.reference("out")
                )
            ],
        )
    )
    document = flow.ModuleDocument("root")
    document.insert(builder)
    result = await document.compile().main.invoke(registry=ActionRegistry())
    assert result["out"] == "ready"


@pytest.mark.asyncio
async def test_direct_import_implements_flow_local_contract():
    program = flow.loads(
        """flow main {
          declare flow render { out text: string }
          import "library" as library
from library import render
          out report: string
          (run render()).text -> report
        }""",
        "root",
        modules={
            "library": (
                'flow render exported { out text: string\n "ok" -> text }'
            )
        },
    )
    assert await program.main.invoke(registry=ActionRegistry()) == {
        "report": "ok"
    }


@pytest.mark.asyncio
async def test_local_contract_resolves_module_implementation():
    program = flow.loads("""
flow main {
  declare flow render { out text: string }
  out report: string
  (run render()).text -> report
}
flow render { out text: string
 "ok" -> text
}
""")
    assert await program.main.invoke(registry=ActionRegistry()) == {
        "report": "ok"
    }


def test_local_struct_contract_checks_module_implementation():
    with pytest.raises(FlowSyntaxError):
        flow.loads("""
flow wrapper {
 struct Local { text: string required }
 declare flow external { in value: Local required
   out text: string }
 out report: string
 (run external(value: Local{"text": "ok"})).text -> report
}
flow external { in value: object required
 out text: string
 value | map it.text -> text
}
""")


@pytest.mark.asyncio
async def test_local_declaration_resolves_external_registry_name():
    registry = ActionRegistry()
    implementation = flow.loads("""flow external { out result: string
 "ok" -> result
}""")
    implementation.register_all(registry)
    caller = flow.loads("""flow main {
 declare flow external { out result: string }
 out report: string
 (run external()).result -> report
}""")
    assert await caller.main.invoke(registry=registry) == {"report": "ok"}


def test_direct_import_schema_conflict_reports_local_declaration():
    with pytest.raises(FlowSyntaxError, match="contracts conflict"):
        flow.loads(
            """flow main {
 declare flow render { out result: string }
 import "library" as library
from library import render
 run render()
}""",
            "root",
            modules={"library": "flow render exported { out result: int }"},
        )


def test_direct_import_does_not_auto_register_imported_implementation():
    program = flow.loads(
        """declare flow render {}
import "library" as library
from library import render
flow main {}""",
        "root",
        modules={"library": "flow render exported {}"},
    )
    assert program.main.name == "main"
    registry = ActionRegistry()
    program.register_all(registry)
    assert registry.is_registered("main")
    assert not registry.is_registered("render")


@pytest.mark.asyncio
async def test_flow_local_struct_roundtrips_values():
    program = flow.loads("""flow main {
      struct Report { text: string required }
      out report: Report
      Report{"text": "ok"} -> report
    }""")
    result = await program.main.invoke(registry=ActionRegistry())
    assert result["report"].text == "ok"


def test_builders_emit_local_declarations_structs_and_symbol_imports():
    builder = flow.FlowBuilder("main")
    interface = flow.FlowBuilder("render", interface_only=True)
    builder.add_declaration(interface)
    shape = flow.StructBuilder("Report")
    shape.field("text", flow.Type("string"))
    builder.add_struct(shape)
    builder.add_import("library", "library")
    builder.add_import("library", "render", symbol="render")
    source = builder.emit()
    assert "declare flow render" in source
    assert "struct Report" in source
    assert "from library import render" in source
    program = flow.loads(
        source, "root", modules={"library": "flow render exported {}"}
    )
    assert program.main.name == "main"


@pytest.mark.asyncio
@pytest.mark.parametrize("stage", ["take while it < 3", "take until it >= 3"])
async def test_predicate_prefixes_and_after_blocks_roundtrip(stage):
    program = flow.loads(f"""flow main {{
 in values: integer stream
 out kept: integer stream
 out cleaned: string
 done = {{ values | {stage} -> kept }}
 after done {{ "done" -> cleaned }}
}}""")
    assert await program.main.invoke(
        values=[1, 2, 3, 1], registry=ActionRegistry()
    ) == {"kept": [1, 2], "cleaned": "done"}


@pytest.mark.asyncio
async def test_after_builder_accepts_multiple_completion_subjects():
    expression = flow.Expression
    builder = flow.FlowBuilder("main")
    builder.port("result", flow.Type("integer"), output=True, unary=False)
    values = expression.array([expression.literal(1), expression.literal(2)])
    values = values.pipeline("flatten").pipeline(
        "take until", expression.literal(False)
    )
    builder.add(
        flow.Statement.after(
            [expression.invoke("gate", {}), expression.invoke("gate", {})],
            [flow.Statement.pipe(values, expression.reference("result"))],
        )
    )
    document = flow.ModuleDocument("root")
    document.insert(flow.FlowBuilder("gate"))
    document.insert(builder)
    result = await document.compile()["main"].invoke(registry=ActionRegistry())
    assert result == {"result": [1, 2]}


@pytest.mark.asyncio
async def test_after_block_gates_nested_action_arguments():
    import asyncio

    releases = [asyncio.Event(), asyncio.Event()]
    started = [asyncio.Event(), asyncio.Event()]
    completed = [asyncio.Event(), asyncio.Event()]
    touched = []
    registry = ActionRegistry()

    def gate(index):
        async def wait(action):
            started[index].set()
            await releases[index].wait()
            completed[index].set()

        return wait

    async def inner(action):
        assert all(event.is_set() for event in completed)
        touched.append("inner")
        await (await action["result"].put("ready"))

    async def outer(action):
        assert all(event.is_set() for event in completed)
        touched.append("outer")
        assert await action["value"].consume(str) == "ready"

    for index in range(2):
        name = f"gate{index}"
        registry.register(
            name, ActionSchema.model_validate({"name": name}), gate(index)
        )
    registry.register(
        "inner",
        ActionSchema.model_validate(
            {
                "name": "inner",
                "outputs": {"result": {"type": str, "unary": True}},
            }
        ),
        inner,
    )
    registry.register(
        "outer",
        ActionSchema.model_validate(
            {"name": "outer", "inputs": {"value": {"type": str, "unary": True}}}
        ),
        outer,
    )
    program = flow.loads("""flow main {
 first = run gate0()
 second = run gate1()
 after first, second {
   run outer(value: (run inner()).result)
 }
}""")
    task = asyncio.create_task(program.main.invoke(registry=registry))
    try:
        await asyncio.wait_for(
            asyncio.gather(*(event.wait() for event in started)), 3
        )
        assert touched == []
        releases[0].set()
        await asyncio.wait_for(completed[0].wait(), 3)
        assert touched == []
        releases[1].set()
        await asyncio.wait_for(task, 3)
    finally:
        for event in releases:
            event.set()
        if not task.done():
            task.cancel()
        await asyncio.gather(task, return_exceptions=True)
    assert sorted(touched) == ["inner", "outer"]


@pytest.mark.asyncio
async def test_plain_pipeline_binding_joins_destinations_and_children():
    program = flow.loads("""flow emit { out values: int stream
 [1, 2, 3, 1] | flatten -> values
}
flow main {
 out left: int stream
 out right: int stream
 out completed: bool
 forwarded = (run emit()).values
   | take until it == 3
   -> left, right
 after forwarded { status forwarded | map it.ok -> completed }
}""")
    assert await program["main"].invoke(registry=ActionRegistry()) == {
        "left": [1, 2],
        "right": [1, 2],
        "completed": True,
    }


def test_native_standard_action_schemas_match_bundled_catalogue():
    from a11._native.flow import request, standard_action_schemas

    bundled = request({"method": "catalogue"})["result"]
    actions = {item["name"]: item for item in bundled["actions"]}
    for schema in standard_action_schemas():
        assert schema.name in actions
        for side in ("inputs", "outputs"):
            expected = getattr(schema, side)
            actual = {
                port["name"]: port
                for port in actions[schema.name].get(side, [])
            }
            assert actual.keys() == expected.keys()
            for name, port in expected.items():
                assert actual[name]["type"] == port.type
                assert actual[name].get("unary", True) == port.unary
                assert actual[name].get("required", False) == port.required


def test_scanning_native_sources_preserves_standard_stream_analysis():
    from pathlib import Path
    from a11._native.flow import request, scan

    root = Path(__file__).resolve().parents[3]
    context = scan([str(root / "cpp/sdk/flow/actions/system_actions.cc")])
    assert all(not item["schema_complete"] for item in context["actions"])
    source = """flow f {
 stdin = run read_stdin()
 {"command": "stop"} -> stdin.control_events
 stdout = run write_stdout()
 "ready" -> stdout.content
}"""
    result = request({"method": "check", "source": source, "context": context})
    assert result["ok"]
    assert not [
        d for d in result["result"]["diagnostics"] if d["severity"] == "error"
    ]
    hover = request(
        {
            "method": "describe",
            "source": source,
            "offset": source.index("write_stdout"),
            "context": context,
        }
    )
    assert hover["ok"]
    assert "content" in str(hover["result"])


@pytest.mark.asyncio
async def test_status_values_preserve_native_details_across_flow_ports():
    from a11.status import Status

    program = flow.loads("""flow main {
      in original: std::status
      out copied: std::status
      std::status value = take original
      value -> copied
    }""")
    original = Status(
        code=StatusCode.NOT_FOUND,
        message="missing",
        details=[{"resource": "page", "id": 7}],
    )
    result = await program.main.invoke(
        original=original, registry=ActionRegistry()
    )
    assert result["copied"].code == original.code
    assert result["copied"].message == original.message
    assert result["copied"].details == original.details


def test_document_compilation_uses_host_source_roots_and_limits(tmp_path):
    (tmp_path / "library.flow").write_text("flow work exported {}")
    document = flow.ModuleDocument(
        str(tmp_path / "root.flow"),
        'import "library.flow"\nflow main { run library::work() }',
    )
    with pytest.raises(StatusException, match="imports are disallowed"):
        document.compile()
    assert document.compile(source_roots=[str(tmp_path)]).main.name == "main"
    limits = flow.CompilationLimits()
    limits.max_work = 0
    with pytest.raises(StatusException):
        document.compile(source_roots=[str(tmp_path)], limits=limits)


@pytest.mark.asyncio
async def test_whole_stream_pipeline_invocations_and_builder():
    source = flow.Expression.reference("items").pipeline_invoke("clean")
    assert "| run clean()" in source.source
    program = flow.loads("""
flow clean {
  in input: string stream
  out output: string stream
  input | map std::trim(it) | where std::len(it) > 0 -> output
}
flow main {
  in items: string stream
  out result: string stream
""" + source.source + " -> result\n}")
    result = await program["main"].invoke(
        {"items": [" a ", "", "b"]}, registry=ActionRegistry()
    )
    assert result == {"result": ["a", "b"]}


def test_missing_qualified_callable_reaches_editor_diagnostics():
    source = "flow main { run nested::test() }"
    checked = flow.request({"method": "check", "source": source})
    diagnostics = [
        d
        for d in checked["result"]["diagnostics"]
        if d["code"] == "flow.name.unknown-call"
    ]
    assert len(diagnostics) == 1
    assert diagnostics[0]["severity"] == "error"
    with pytest.raises(FlowSyntaxError, match="nested::test"):
        flow.loads(source)


@pytest.mark.asyncio
async def test_pipeline_call_preserves_peer_dispatch_with_a_local_handler():
    import asyncio
    import contextlib

    import a11
    from a11 import net

    template = """
flow external {
 in input: string stream
 out output: string stream
 input | map std::TRANSFORM(it) -> output
}
"""
    theirs = ActionRegistry()
    ours = ActionRegistry()
    flow.loads(template.replace("TRANSFORM", "upper")).register_all(theirs)
    flow.loads(template.replace("TRANSFORM", "lower")).register_all(ours)
    service = a11.Service(action_registry=theirs)
    server, client = net.create_in_process_wire_stream_pair()
    serving = asyncio.ensure_future(service.accept(server))
    session = a11.Session(action_registry=ours)
    await session.add_stream(client, mode="start")
    source = """
declare flow external {
 in input: string stream
 out output: string stream
}
flow main {
 out output: string stream
 ["Ab", "Cd"] | flatten | call external() -> output
}
"""
    try:
        peer = flow.loads(source)
        result = await peer.main.invoke(
            registry=ours, session=session, dispatch_stream=client
        )
        assert result == {"output": ["AB", "CD"]}
        local = flow.loads(source.replace("| call external", "| run external"))
        assert await local.main.invoke(registry=ours) == {
            "output": ["ab", "cd"]
        }
    finally:
        with contextlib.suppress(Exception):
            await client.close()
        service.abort(
            a11.Status(code=a11.StatusCode.CANCELLED, message="test finished")
        )
        with contextlib.suppress(Exception):
            await asyncio.wait_for(serving, timeout=10)


def test_pipeline_invocation_hover_and_plan_describe_stream_adapter():
    source = """
flow clean { in input: string stream
 out output: string stream
 input -> output
}
flow main { in items: string stream
 out result: string stream
 items | run clean() -> result
}
"""
    description = flow.request(
        {
            "method": "describe",
            "source": source,
            "offset": source.rindex("clean()"),
        }
    )
    assert "Whole-stream invocation" in description["result"]["markdown"]
    plan = flow.request({"method": "plan", "source": source})["result"]
    main = next(item for item in plan["flows"] if item["flow"] == "main")
    call = next(step for step in main["steps"] if "pipeline" in step)
    assert call["mode"] == "run"
    assert call["pipeline"] == {
        "input": "input",
        "output": "output",
        "activation": "whole-stream",
    }


@pytest.mark.asyncio
async def test_scope_barrier_builder_and_runtime():
    barrier = flow.Statement.wait_all()
    assert barrier.source == "wait all"
    program = flow.loads("""
flow main {
 out result: int
 x = 1
""" + barrier.source + "\n x = x + 1\n x -> result\n}")
    assert await program.main.invoke(registry=ActionRegistry()) == {"result": 2}


def test_scope_barrier_tooling_envelopes_and_incomplete_input():
    source = "flow main { x = 1\n wait all\n x -> _\n}"
    syntax = flow.request({"method": "parse", "source": source})
    assert "scope_barrier" in str(syntax)
    plan = flow.request({"method": "plan", "source": source})["result"]
    steps = plan["flows"][0]["steps"]
    barrier = next(step for step in steps if step.get("scope_barrier"))
    assert barrier["label"] in steps[-1]["after"]
    hover = flow.request(
        {"method": "describe", "source": source, "offset": source.index("wait")}
    )["result"]
    assert "all preceding work in this scope" in hover["markdown"]
    for suffix in ("wait", "wait all", "wait all of", "wait all timeout"):
        checked = flow.request(
            {"method": "check", "source": "flow main { " + suffix}
        )
        assert checked["result"]["format"] == "flow.diagnostics/v1"
    invalid = flow.request(
        {"method": "check", "source": "flow main { x = wait all }"}
    )["result"]
    assert any(
        d["code"] == "flow.form.scope-barrier" for d in invalid["diagnostics"]
    )
