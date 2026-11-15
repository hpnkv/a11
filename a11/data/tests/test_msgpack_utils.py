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

import array

import pytest

from a11.data.msgpack_utils import ensure_memoryview, unpackb
from a11.status import StatusException


def test_unpackb_handles_empty_and_nil_buffers():
    assert unpackb(b"\xc0", str, allow_none=True) is None
    with pytest.raises(StatusException):
        unpackb(b"", str, allow_none=True)
    with pytest.raises(StatusException):
        unpackb(b"\xc0x", str, allow_none=True)


def test_memoryview_length_is_in_bytes():
    view = ensure_memoryview(memoryview(b"\xa2ok")[::2])
    assert view.tobytes() == b"\xa2k"
    assert len(view) == 2
    words = memoryview(array.array("H", [1]))
    assert len(ensure_memoryview(words)) == words.nbytes
