#!/usr/bin/env bash
# A11 prose style, as a hook. Two modes:
#
#   context  SessionStart: state the style in the session's context.
#   filter   PreToolUse on Write|Edit: refuse text carrying the phrases the
#            guide names. A backstop under the guide, not a substitute for it.
#
# The phrase list and the guide live here together so they cannot drift.
set -uo pipefail

mode=${1:-filter}
payload=$(cat)

banned='load-bearing|deliberately|on purpose|the whole point|smoking gun|blast radius|substrate|earns its place|earn their place|worth stating|worth noting|it is worth|it'"'"'s worth|rubber duck|better than|faster than|simpler than|superior to|drop-in replacement|the best choice'

read -r -d '' instruction <<'GUIDE' || true
A11 writing style. Applies to comments, docstrings, guides, commit messages and
replies.

VOICE: DESCRIBE THE ARTEFACT
The subject is the code or the constraint; the verb is present tense and
factual -- requires, provides, returns, supports, exposes, remains, uses,
omits. Do not give code intentions, appetites or knowledge, and do not invoke
an unnamed person to carry the point.

  no:  A port whose type nobody stated is described without one.
  yes: A port with no stated type is described without one.

  no:  These autofills deliberately never travel.
  yes: Receiver-owned autofill defaults remain local.

  no:  What cannot survive the trip does not.
  yes: A port's local typeinfo handle returns as None.

DELETE, DO NOT REPHRASE
Four kinds of sentence have no shorter correct form. Remove them.

1. History -- what the code used to do, which earlier attempt failed, what an
   earlier commit or findings document asked for.

   no:  This is the loop that used to exist twice, once in the CLI and once in
        the webview, and in the CLI's case ran the model call in-process.
   yes: This module owns the non-presentation workflow.

2. Counterfactual warnings -- what would break, what can no longer happen,
   which bug a plainer version reintroduces.

   no:  Clear it before the unlock, or the next holder's store is lost.
   yes: Cleared before the unlock.

3. Intent emphasis -- deliberately, on purpose, the whole point, which is why,
   that is what.

4. Value and cost framing -- worth stating, worth noting, load-bearing, buys,
   pays, earns its place, cheap or expensive as praise.

   no:  A type that will not describe itself is worth a line and nothing more:
        the action still works, and the alternative is refusing to register it.
   yes: Schema derivation is optional metadata. Keep the action registered and
        record the failure for diagnostics.

Measurement narratives and profile tables belong in a benchmark. Give the
number, or name the benchmark that produces it.

STATE WHAT IS
Avoid "not X but Y", "rather than" as rhetoric, and aphorism.

  no:  Two details are load-bearing rather than stylistic:
  yes: The following ordering and lifecycle requirements are part of the
       contract:

HEADINGS
Imperative for a task or guide section; noun phrase for a reference or API
section. Never a rhetorical question, second-person framing, an
X-rather-than-Y construction, or a value qualifier.

  no:  Reading a stream you do not want    yes: Discarding stream values
  no:  What you get for free               yes: A11 executables and the CLI
  no:  Checking rather than assuming       yes: Check whether the allocator is
                                                active
  no:  What just happened                  yes: How the tool call is handled

Python module docstrings open in the imperative: "Drive one conversational
turn", not "Driving one conversational turn".

THE READER
Address the reader in task framing that names their situation: "Give a model
tools you already use", "If A11 is new to you, start with an action". Keep the
reader out of explanations of how the code behaves.

LENGTH
Implementation comments: at most three lines and 80 columns, for unconventional
paths and complex decisions only. API docstrings keep parameter semantics,
nuanced behaviour and in-context examples, and may run longer. Operational
comments may exceed three lines when the instructions need the space --
generated-file warnings, regeneration commands, required call ordering,
ignored-work explanations. Licence headers stay verbatim; keep namespace and
header-guard closing comments.

DOCUMENTATION SHAPE
Lead with what the reader can do, not with a classification of the product,
unless to give intruductions in the more-general-by-nature descriptions such as
on the landing page.

  no:  A11 is a concurrent action and streaming runtime for AI agents that run
       in one process or span multiple machines.
  yes: A11 helps an agent start work immediately, stream partial results, and
       call the same operations in another process or on another machine.

A landing page is a task menu: a heading such as "Choose what you want to
build", then linked task phrases with one sentence of scope each. Name the
reader's situation and the problem removed, not the bare operation.

  no:  Stream a response from a language model
  yes: Stream text without handling provider events

Carry one named minimal example, such as "A stream in one minute". Examples
import the public surface (import a11, then a11.AsyncNode) rather than a deep
module path, and carry no inline explanatory comments -- the prose above the
block explains. Define a concept inline in bold, then what it does, then when
to reach for it.

COMPARISONS
Orient by comparison to a named product, linked to its own documentation, in
this order: name and link it; state what it does and when it is effective,
accurately; then the specific difference. Grant the alternative its strengths
and state A11's own limits. No competitive superlatives.

  no:  A11 is faster than gRPC and needs no service definition.
  yes: gRPC starts from a service definition with generated client and server
       code, and is effective when services have a stable interface and every
       client can build against it. A11 retains efficient connection reuse and
       makes the operation layer dynamic.
  yes: SQLite is primarily a one-machine choice; Redis Cluster needs the
       routing setup described in the Redis reference.

Widen the audience with concrete products rather than features: serving a GPU
model, live captions from speech recognition, an indexing pipeline, diffusion
progress before an image.

The filter mode of this hook refuses a Write or Edit carrying the phrases named
above.
GUIDE

if [ "$mode" = "context" ]; then
  jq -n --arg text "$instruction" '{
    hookSpecificOutput: {
      hookEventName: "SessionStart",
      additionalContext: $text
    }
  }'
  exit 0
fi

path=$(printf '%s' "$payload" | jq -r '.tool_input.file_path // ""')

# The files that define the ban have to be able to name the phrases.
case "$path" in
  */scripts/hook_writing_style.sh|*/doc/docs/contributing/writing-style.md) exit 0 ;;
esac

case "$path" in
  *.md|*.cc|*.cpp|*.h|*.hpp|*.py|*.pyi|*.txt|*.yml|*.yaml|*.ts|*.tsx|*.js|*.kt|*.rs|*.sh|*.flow|*CMakeLists.txt) ;;
  *) exit 0 ;;
esac

added=$(printf '%s' "$payload" | jq -r '
  [.tool_input.content? // empty, .tool_input.new_string? // empty] | join("\n")')
[ -n "$added" ] || exit 0

hits=$(printf '%s' "$added" | grep -oiE "$banned" | sort -u | paste -sd', ' -)
[ -n "$hits" ] || exit 0

jq -n --arg hits "$hits" '{
  hookSpecificOutput: {
    hookEventName: "PreToolUse",
    permissionDecision: "deny",
    permissionDecisionReason: ("A11 writing style: remove " + $hits +
      ". Describe the artefact with a present-tense factual verb. Delete history, counterfactual warnings, intent emphasis and value framing rather than rephrasing them. See the style guide injected at session start, and AGENTS.md.")
  }
}'
