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

"""Reduce model-facing context through opt-in policies and ordinary actions."""

from a11.sdk.llm_context.actions import register
from a11.sdk.llm_context.adapters import (
    claude_code_hooks,
    codex_tool_hook,
    compact_acp_session,
    compact_claude_session,
    compact_codex_thread,
    project_acp_prompt,
)
from a11.sdk.llm_context.config import (
    LlmContextConfig,
    LlmContextHeaders,
    LlmContextScope,
    LlmContextServices,
    bind_services,
    configure_registry,
    decode_config,
)
from a11.sdk.llm_context.content import (
    compress_content,
    count_context,
    expand_content,
)
from a11.sdk.llm_context.harness import compact_host_history
from a11.sdk.llm_context.history import compact_history, compact_interactions
from a11.sdk.llm_context.interaction_owner import (
    accept_interaction_view,
    compact_host_interactions,
    load_interaction_view,
    persist_interaction_view,
)
from a11.sdk.llm_context.interactions import (
    decode_interactions,
    encode_interactions,
    interaction_view_sources,
    project_interactions,
    use_interaction_view,
    validate_interaction_view,
)
from a11.sdk.llm_context.measurement import (
    RunRecorder,
    compare_runs,
    evaluate_pair,
    load_runs,
)
from a11.sdk.llm_context.retention import RetainedContentStore
from a11.sdk.llm_context.runtime import project_messages, project_view
from a11.sdk.llm_context.shell import intercept_shell_result, shell_hook

__all__ = [
    "register",
    "LlmContextConfig",
    "LlmContextHeaders",
    "LlmContextServices",
    "LlmContextScope",
    "RetainedContentStore",
    "intercept_shell_result",
    "shell_hook",
    "bind_services",
    "configure_registry",
    "decode_config",
    "compress_content",
    "count_context",
    "expand_content",
    "compact_history",
    "compact_host_history",
    "project_messages",
    "project_view",
    "claude_code_hooks",
    "codex_tool_hook",
    "compact_acp_session",
    "compact_claude_session",
    "compact_codex_thread",
    "project_acp_prompt",
    "compact_interactions",
    "project_interactions",
    "accept_interaction_view",
    "compact_host_interactions",
    "encode_interactions",
    "decode_interactions",
    "interaction_view_sources",
    "load_interaction_view",
    "persist_interaction_view",
    "use_interaction_view",
    "validate_interaction_view",
    "RunRecorder",
    "compare_runs",
    "evaluate_pair",
    "load_runs",
]
