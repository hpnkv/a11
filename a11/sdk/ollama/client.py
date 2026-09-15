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

import hashlib
import os
from typing import Any

from ollama import AsyncClient, ChatResponse
from ollama._types import ChatRequest


class _CacheAwareChatResponse(ChatResponse):
    """Ollama response fields used by A11 ahead of an SDK release."""

    prompt_eval_cached_count: int | None = None


class _CacheAwareAsyncClient(AsyncClient):
    """Keep Ollama's cached prompt count in streamed response objects."""

    async def chat(self, *args: Any, **kwargs: Any) -> Any:
        if "prompt_eval_cached_count" in ChatResponse.model_fields:
            return await super().chat(*args, **kwargs)

        bound = {
            "model": args[0] if args else kwargs.pop("model", ""),
            "messages": (
                args[1] if len(args) > 1 else kwargs.pop("messages", None)
            ),
            "tools": kwargs.pop("tools", None),
            "stream": kwargs.pop("stream", False),
            "think": kwargs.pop("think", None),
            "format": kwargs.pop("format", None),
            "options": kwargs.pop("options", None),
            "keep_alive": kwargs.pop("keep_alive", None),
        }
        request = ChatRequest(**bound, **kwargs)
        return await self._request(
            _CacheAwareChatResponse,
            "POST",
            "/api/chat",
            json=request.model_dump(exclude_none=True),
            stream=bound["stream"],
        )


def get_ollama_client(
    host: str | None = None, api_key: str | None = None
) -> AsyncClient:
    """Return a cached Ollama `AsyncClient` for a (host, api key) pair.

    Ollama typically runs locally and needs no credentials, so unlike the
    hosted backends this never raises when a key is absent. `host` falls back to
    `OLLAMA_HOST` (else the SDK's built-in `http://127.0.0.1:11434` default) and
    an `api_key`, if supplied (e.g. for Ollama's hosted service), is sent as a
    bearer token on every request.
    """
    host = host or os.environ.get("OLLAMA_HOST") or None
    api_key = api_key or os.environ.get("OLLAMA_API_KEY", "")

    if not hasattr(get_ollama_client, "_clients"):
        get_ollama_client._clients = {}

    cache_key = hashlib.sha256(f"{host or ''}\0{api_key}".encode()).hexdigest()

    if cache_key not in get_ollama_client._clients:
        headers = {"Authorization": f"Bearer {api_key}"} if api_key else None
        get_ollama_client._clients[cache_key] = _CacheAwareAsyncClient(
            host=host, headers=headers
        )

    return get_ollama_client._clients[cache_key]
