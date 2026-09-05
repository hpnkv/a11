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

import asyncio, time, os, tempfile
from a11 import net
from a11.net import http

PAYLOAD = b"x" * (128 * 1024 * 1024)


async def main():
    async def handler(request, response):
        response.send_response(
            200, [("content-length", str(len(PAYLOAD)))], PAYLOAD
        )

    server = net.Http2Server.create("127.0.0.1", 0, handler)
    try:
        url = f"http://127.0.0.1:{server.port}/big"
        with tempfile.TemporaryDirectory() as d:
            for i in range(3):
                n = [0]

                def prog(a, b):
                    n[0] += 1

                o = http.DownloadOptions()
                o.destination = os.path.join(d, f"f{i}")
                o.on_progress = prog
                t0 = time.time()
                p = await http.download(url, o)
                dt = time.time() - t0
                sz = os.path.getsize(p)
                print(
                    f"  loopback dl {i+1}: {sz/dt/1e6:.0f} MB/s  chunks={n[0]} "
                    f" avg={sz//max(n[0],1)//1024} KiB"
                )
    finally:
        server.stop()


asyncio.run(main())
