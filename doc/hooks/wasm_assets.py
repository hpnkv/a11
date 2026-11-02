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

"""Copy the tested WASM SDK assets into MkDocs builds and live previews."""

from pathlib import Path
from shutil import copytree


ASSETS = Path(__file__).resolve().parents[2] / "js" / "dist" / "wasm"


def on_post_build(config, **_kwargs):
    """Serve the same content-hashed assets in preview and release builds."""
    if not (ASSETS / "asset-manifest.json").is_file():
        raise FileNotFoundError(f"Missing packaged A11 WASM assets: {ASSETS}")
    copytree(ASSETS, Path(config.site_dir) / "assets" / "wasm", dirs_exist_ok=True)


def on_serve(server, **_kwargs):
    """Rebuild the preview when the packaged WASM asset set changes."""
    server.watch(str(ASSETS))
    return server
