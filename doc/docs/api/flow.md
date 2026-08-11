# Flow language

Flow is a small language for describing a composition of actions that is itself
an action. See [Compose actions without deploying code](../guides/flow.md) for a
walk through one, and the `a11.flow.REFERENCE` constant for the cheat sheet to
put in front of a model that has to write one.

::: a11.flow

## Compiling

[`loads`][a11.flow.loads] compiles source that arrived as a string,
[`load`][a11.flow.load] a `.flow` file, and [`register`][a11.flow.register] does
both and publishes the result as actions in one call. A problem raises
[`FlowSyntaxError`][a11.flow.lexer.FlowSyntaxError], which carries the line and
column and converts to an A11 status.

::: a11.flow.loads

::: a11.flow.load

::: a11.flow.register

::: a11.flow.lexer.FlowSyntaxError

## Programs and flows

::: a11.flow.plan.Program

::: a11.flow.plan.FlowPlan

## The compiled graph

A compiled flow is data: [`describe`][a11.flow.plan.FlowPlan.describe] renders
the whole composition, which is what makes one reviewable before it is run.

::: a11.flow.plan

## Running one

::: a11.flow.runtime
