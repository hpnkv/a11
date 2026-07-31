# Copyright 2026 The A11 Authors.

import hashlib
import os

from google import genai

from a11.status import Status, StatusCode


def get_gemini_client(api_key: str | None = None) -> genai.Client:
    if not api_key:
        api_key = os.environ.get("GEMINI_API_KEY", "") or os.environ.get(
            "GOOGLE_API_KEY", ""
        )
    if not api_key:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="GEMINI_API_KEY is not set, nor is a key supplied.",
        ).to_exception()

    if not hasattr(get_gemini_client, "_clients"):
        get_gemini_client._clients = {}

    api_key_hash = hashlib.sha256(api_key.encode()).hexdigest()

    if api_key_hash not in get_gemini_client._clients:
        get_gemini_client._clients[api_key_hash] = genai.Client(api_key=api_key)

    return get_gemini_client._clients[api_key_hash]
