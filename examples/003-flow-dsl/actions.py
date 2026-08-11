"""The actions the example flows compose.

Ordinary A11 actions, written the way any of them would be: schema-described,
streaming, and unaware that anything is composing them. A tiny in-memory corpus
stands in for a search index and a fetcher, and ``llm-summarize`` stands in for
a model -- it reports how much text it was given, which is the number the flows
in this example are trying to keep small.
"""

from __future__ import annotations

import asyncio
from typing import Any

from a11.actions import Action, ActionRegistry, ActionSchema

#: A pretend index: what `web-search` finds and `web-fetch` returns.
CORPUS: dict[str, dict[str, Any]] = {
    "https://example.test/fibers": {
        "title": "Fibers and why A11 uses them",
        "words": 320,
        "text": (
            "A fiber is a stack that a scheduler can park. A11 runs its "
            "cooperative work on fibers so that a synchronous-looking call can "
            "wait without blocking a thread. "
        ),
    },
    "https://example.test/nodes": {
        "title": "Nodes are streams with a store behind them",
        "words": 275,
        "text": (
            "An AsyncNode has a writer half and a reader half over one chunk "
            "store. Values are appended as chunks and read back in order, "
            "which is what lets two actions stream through each other. "
        ),
    },
    "https://example.test/actions": {
        "title": "Actions, ports, and dispatch",
        "words": 410,
        "text": (
            "An action names its input and output ports, and each port is a "
            "node. Dispatching one over a session mirrors its nodes on the "
            "peer, so a caller reads outputs as they are produced. "
        ),
    },
    "https://example.test/unrelated": {
        "title": "A page about bicycles",
        "words": 90,
        "text": "Bicycles have two wheels and no relevance here. ",
    },
}


SEARCH = ActionSchema.model_validate(
    {
        "name": "web-search",
        "description": "Find pages matching a query.",
        "inputs": {
            "query": {"type": str, "unary": True, "required": True},
            "limit": {"type": int, "unary": True},
        },
        "outputs": {
            "hits": {"type": dict},
            "debug": {"type": str},
        },
    }
)

FETCH = ActionSchema.model_validate(
    {
        "name": "web-fetch",
        "description": "Fetch one page as text.",
        "inputs": {"url": {"type": str, "unary": True, "required": True}},
        "outputs": {
            "text": {"type": str, "unary": True},
            "bytes": {"type": int, "unary": True},
        },
    }
)

SUMMARIZE = ActionSchema.model_validate(
    {
        "name": "llm-summarize",
        "description": "Answer a question from the pages it is given.",
        "inputs": {
            "question": {"type": str, "unary": True, "required": True},
            "pages": {"type": str},
        },
        "outputs": {
            "summary": {"type": str, "unary": True},
            "characters": {"type": int, "unary": True},
        },
    }
)

NOTES = ActionSchema.model_validate(
    {
        "name": "take-notes",
        "description": "Summarise pages, noting progress as it goes.",
        "inputs": {"pages": {"type": str, "unary": False}},
        "outputs": {"summary": {"type": str, "unary": True}},
    }
)

TRIAGE_STEP = ActionSchema.model_validate(
    {
        "name": "triage-step",
        "description": "One pass of a triage loop over a report.",
        "inputs": {"state": {"type": dict, "unary": True, "required": True}},
        "outputs": {"next": {"type": dict, "unary": True}},
    }
)


async def web_search(action: Action) -> None:
    query = await action["query"].consume(str)
    limit = await action["limit"].consume(int, allow_none=True) or 3
    await (await action["debug"].put(f"searching for {query!r}"))
    terms = [term for term in query.lower().split() if len(term) > 3]
    found = 0
    for url, page in CORPUS.items():
        haystack = f"{url} {page['title']} {page['text']}".lower()
        if not any(term in haystack for term in terms):
            continue
        await (await action["hits"].put({"url": url, "title": page["title"]}))
        found += 1
        if found >= limit:
            break
    await (await action["debug"].put(f"found {found} page(s)"))


async def web_fetch(action: Action) -> None:
    url = await action["url"].consume(str)
    page = CORPUS.get(url)
    if page is None:
        raise ValueError(f"no such page: {url}")
    # A real fetch is slow and returns a lot; both matter to the flows.
    await asyncio.sleep(0.05)
    text = page["text"] * 8
    await (await action["text"].put(text))
    await (await action["bytes"].put(len(text)))


async def llm_summarize(action: Action) -> None:
    question = await action["question"].consume(str)
    pages: list[str] = []
    async for page in action["pages"]:
        pages.append(page)
    characters = sum(len(page) for page in pages)
    first_lines = [page.strip().split(".")[0] for page in pages if page.strip()]
    await (
        await action["summary"].put(
            f"On {question!r}, from {len(pages)} page(s): "
            + "; ".join(first_lines)
            + "."
        )
    )
    await (await action["characters"].put(characters))


async def take_notes(action: Action) -> None:
    """Report progress into whatever node the caller named."""
    where = action.get_header("x-a11-progress-node", decode=True)
    progress = action.get_node(where) if where else None
    seen = 0
    async for page in action["pages"]:
        seen += 1
        if progress is not None:
            await (
                await progress.put(f"read page {seen} ({len(page)} chars)")
            )
    await (
        await action["summary"].put(f"took notes on {seen} page(s)")
    )


async def triage_step(action: Action) -> None:
    """Advance a small state machine, the way an agent step would."""
    state = await action.get_input("state").consume(dict)
    round_number = int(state.get("round", 0)) + 1
    notes = list(state.get("notes", []))
    notes.append(f"pass {round_number}")
    confidence = round(0.4 * round_number, 2)
    await (
        await action.get_output("next").put(
            {
                "round": round_number,
                "notes": notes,
                "confidence": confidence,
                "done": confidence >= 0.8,
                "verdict": (
                    "needs a human" if round_number > 2 else "handled"
                ),
            }
        )
    )


def make_registry() -> ActionRegistry:
    """A registry holding the actions the example flows call."""
    registry = ActionRegistry()
    registry.register("web-search", SEARCH, web_search)
    registry.register("web-fetch", FETCH, web_fetch)
    registry.register("llm-summarize", SUMMARIZE, llm_summarize)
    registry.register("take-notes", NOTES, take_notes)
    registry.register("triage-step", TRIAGE_STEP, triage_step)
    return registry


def raw_corpus_size() -> int:
    """How much text the pages hold, before any flow trims them."""
    return sum(len(page["text"]) * 8 for page in CORPUS.values())
