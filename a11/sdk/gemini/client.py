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
