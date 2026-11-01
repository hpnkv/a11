"""Verify storage-qualified references against the shared wire fixtures."""

import json
from pathlib import Path

import pytest

import a11
from a11.status import StatusException

CASES = json.loads(
    (Path(__file__).parents[3] / "testdata/node_refs.json").read_text()
)


@pytest.mark.parametrize("case", CASES, ids=lambda case: case["name"])
def test_node_reference_wire_contract(case):
    reference = a11.NodeRef.model_validate(case["json"])
    assert reference.to_msgpack().hex() == case["msgpack_hex"]
    assert (
        a11.NodeRef.from_msgpack(bytes.fromhex(case["msgpack_hex"]))
        == reference
    )
    assert (
        a11.NodeRef.model_validate_json(reference.model_dump_json())
        == reference
    )
    assert reference.model_dump(mode="json") == case["json"]


def test_storage_namespaces_and_credential_resolver():
    first = a11.NodeMap(storage=a11.StorageIdentity("first"))
    second = a11.NodeMap(storage=a11.StorageIdentity("second"))
    host = a11.NodeMap()
    host.register_storage("first", first)
    host.register_storage("second", second)
    assert host.resolve(
        a11.NodeRef("same", storage=a11.StorageIdentity("first"))
    ) is first.get("same")
    assert host.resolve(
        a11.NodeRef("same", storage=a11.StorageIdentity("second"))
    ) is second.get("same")
    assert host.resolve(a11.NodeRef("same")) is host.get("same")
    assert first.get("same") is not second.get("same")
    seen = []

    def resolve(identity):
        seen.append(identity)
        return first if identity.id == "dynamic" else None

    host.set_storage_resolver(resolve, accessible_ids=["dynamic"])
    identity = a11.StorageIdentity(
        "dynamic",
        [
            a11.StorageService(
                "stream",
                "https://storage.example",
                a11.StorageCredentials("bearer", {"token": "test-token"}),
            )
        ],
    )
    assert host.resolve(
        a11.NodeRef("dynamic-node", storage=identity)
    ) is first.get("dynamic-node")
    assert seen[0] == identity
    assert seen[0].services[0].credentials.fields["token"] == "test-token"
    assert "test-token" not in repr(identity)
    assert "test-token" not in repr(a11.NodeRef("node", storage=identity))
    assert "test-token" not in repr(identity.services[0].credentials)
    with pytest.raises(StatusException):
        host.resolve(
            a11.NodeRef("missing", storage=a11.StorageIdentity("unavailable"))
        )
