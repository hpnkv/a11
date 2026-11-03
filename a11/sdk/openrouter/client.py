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

"""Create OpenRouter clients without sharing credentials across endpoints."""

import hashlib
import os

from openai import AsyncOpenAI

from a11.sdk.llm import LlmHeaders
from a11.status import Status, StatusCode

DEFAULT_BASE_URL = "https://openrouter.ai/api/v1"


def get_openrouter_client(
    base_url: str | None = None, api_key: str | None = None
) -> AsyncOpenAI:
    """Use the OpenAI SDK against OpenRouter's Chat Completions route.

    Args:
        base_url: API root, including ``/api/v1``. Defaults to
            ``OPENROUTER_BASE_URL`` or OpenRouter's public endpoint.
        api_key: Bearer key. Defaults to ``OPENROUTER_API_KEY``.
    """
    base_url = (
        base_url or os.environ.get("OPENROUTER_BASE_URL") or DEFAULT_BASE_URL
    ).rstrip("/")
    api_key = api_key or os.environ.get("OPENROUTER_API_KEY")
    if not api_key:
        raise Status(
            code=StatusCode.UNAUTHENTICATED,
            message=(
                f"Set {LlmHeaders.API_KEY.value} or OPENROUTER_API_KEY to use"
                " OpenRouter."
            ),
        ).to_exception()

    if not hasattr(get_openrouter_client, "_clients"):
        get_openrouter_client._clients = {}
    cache_key = hashlib.sha256(f"{base_url}\0{api_key}".encode()).hexdigest()
    if cache_key not in get_openrouter_client._clients:
        get_openrouter_client._clients[cache_key] = AsyncOpenAI(
            base_url=base_url,
            api_key=api_key,
            default_headers={"X-OpenRouter-Metadata": "enabled"},
        )
    return get_openrouter_client._clients[cache_key]
