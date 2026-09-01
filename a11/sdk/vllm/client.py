# Copyright 2026 The A11 Authors.

"""Reach a vLLM deployment through its OpenAI-compatible routes."""

import hashlib
import os
import re

from openai import AsyncOpenAI

#: Where `vllm serve` listens when nothing says otherwise.
DEFAULT_BASE_URL = "http://127.0.0.1:8000/v1"

#: A vLLM server started without ``--api-key`` accepts any credential, and the
#: OpenAI client requires one, so this placeholder stands in.
PLACEHOLDER_API_KEY = "EMPTY"

_VERSION_SEGMENT = re.compile(r"v\d+(?:beta\d*|alpha\d*)?")


def normalize_base_url(base_url: str | None) -> str:
    """The OpenAI-compatible root of a vLLM deployment.

    vLLM serves the OpenAI routes under a version segment (``/v1``). A URL that
    already ends in one is returned as given, and ``/v1`` is appended otherwise,
    so both ``http://gpu-box:8000`` and ``http://gpu-box:8000/v1`` name the same
    deployment.
    """
    base_url = (base_url or "").strip().rstrip("/")
    if not base_url:
        return DEFAULT_BASE_URL
    if _VERSION_SEGMENT.fullmatch(base_url.rsplit("/", 1)[-1]):
        return base_url
    return f"{base_url}/v1"


def get_vllm_client(
    base_url: str | None = None, api_key: str | None = None
) -> AsyncOpenAI:
    """Return a cached `AsyncOpenAI` client for a (base URL, api key) pair.

    A vLLM deployment is self-hosted and usually needs no credentials, so an
    absent key is an ordinary case: `PLACEHOLDER_API_KEY` is sent instead, and a
    server started with ``--api-key`` is reached by supplying the key. Both
    arguments fall back to the environment -- ``VLLM_BASE_URL`` then
    ``OPENAI_BASE_URL`` for the URL, ``VLLM_API_KEY`` then ``OPENAI_API_KEY``
    for the key.
    """
    base_url = normalize_base_url(
        base_url
        or os.environ.get("VLLM_BASE_URL")
        or os.environ.get("OPENAI_BASE_URL")
    )
    api_key = (
        api_key
        or os.environ.get("VLLM_API_KEY")
        or os.environ.get("OPENAI_API_KEY")
        or PLACEHOLDER_API_KEY
    )

    if not hasattr(get_vllm_client, "_clients"):
        get_vllm_client._clients = {}

    cache_key = hashlib.sha256(f"{base_url}\0{api_key}".encode()).hexdigest()

    if cache_key not in get_vllm_client._clients:
        get_vllm_client._clients[cache_key] = AsyncOpenAI(
            base_url=base_url, api_key=api_key
        )

    return get_vllm_client._clients[cache_key]
