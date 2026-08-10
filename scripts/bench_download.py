# Copyright 2026 The A11 Authors.

"""Measure `a11.net.http.download` throughput against a real endpoint.

Reports MB/s and the chunk-size distribution, which is what tells you *why* a
transfer is slow: one chunk per network round trip means a flow-control window
too small for the path, not a slow link. Compare against
``curl -sL -o /dev/null -w '%{speed_download}\\n' <url>`` for a baseline.

    python scripts/bench_download.py [url]

Note the filename: a module called ``http.py`` in this directory would shadow the
standard library's for every script run from here, which breaks imports in
surprising places.
"""

from __future__ import annotations

import asyncio
import os
import sys
import tempfile
import time

from a11.net import http

DEFAULT_URL = (
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"
)


async def main(url: str) -> int:
    deltas: list[int] = []
    last = 0

    def on_progress(done: int, total: int) -> None:
        nonlocal last
        deltas.append(done - last)
        last = done

    with tempfile.TemporaryDirectory() as directory:
        options = http.DownloadOptions()
        options.destination = os.path.join(directory, "payload")
        options.on_progress = on_progress

        started = time.monotonic()
        path = await http.download(url, options)
        elapsed = time.monotonic() - started
        size = os.path.getsize(path)

    chunks = [delta for delta in deltas if delta > 0]
    print(f"{size / 1e6:.1f} MB in {elapsed:.2f}s = {size / elapsed / 1e6:.2f} MB/s")
    if chunks:
        average = sum(chunks) / len(chunks)
        per_chunk_ms = elapsed / len(chunks) * 1000
        print(
            f"{len(chunks)} chunks, avg {average:.0f} B, max {max(chunks)} B,"
            f" {per_chunk_ms:.2f} ms/chunk"
        )
        print(
            "If ms/chunk is close to the round-trip time to the host, the"
            " connection-level flow-control window is the limit, not the link."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(
        asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL))
    )
