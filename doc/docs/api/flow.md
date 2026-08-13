# Flow language

Flow is a small language for describing a composition of actions that is itself
an action. See [Compose actions without deploying code](../guides/flow.md) for a
walk through one, and the `a11.flow.REFERENCE` constant for the cheat sheet to
put in front of a model that has to write one.

::: a11.flow
    options:
      # Everything the package re-exports has its own section below; what
      # belongs to the package itself is the language's own tables.
      members:
        - REFERENCE
        - EXTENSION
        - BUILTINS
        - STAGES
        - FAIL_CODES

## Compiling

[`loads`][a11.flow.loads] compiles source that arrived as a string,
[`load`][a11.flow.load] a `.flow` file, and [`register`][a11.flow.register] does
both and publishes the result as actions in one call. A problem raises
[`FlowSyntaxError`][a11.flow.diagnostics.FlowSyntaxError], which carries the line and
column and converts to an A11 status.

::: a11.flow.loads

::: a11.flow.load

::: a11.flow.register

::: a11.flow.diagnostics.FlowSyntaxError

## Programs and flows

::: a11.flow.plan.Program

::: a11.flow.plan.FlowPlan

## The compiled graph

A compiled flow is data: [`describe`][a11.flow.plan.FlowPlan.describe] renders
the whole composition, which is what makes one reviewable before it is run.

::: a11.flow.plan
    options:
      # The two classes have their own section above; what is left of the module
      # is the type table and the compiler entry point.
      members:
        - TYPE_NAMES
        - compile_source

## Running one

::: a11.flow.runtime

## Diagnostics

Everything that reports on a flow -- the CLI, an editor, a CI job -- renders the
one [`Diagnostic`][a11.flow.diagnostics.Diagnostic] shape. See
[Checking flows from a toolchain](../guides/flow-tooling.md) for the envelopes it
travels in.

::: a11.flow.diagnostics
    options:
      members:
        - DIAGNOSTICS_FORMAT
        - CODES_FORMAT
        - TOKENS_FORMAT
        - PLAN_FORMAT
        - SYNTAX_FORMAT
        - Diagnostic
        - Severity
        - Family
        - Position
        - Range
        - Edit
        - Fix
        - CodeInfo
        - known_codes
        - find_code
        - sort_diagnostics
        - diagnostics_envelope
        - codes_envelope
        - sarif_log
        - LineIndex
