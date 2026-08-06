import asyncio
import random

import pytest

from a11 import timing
from a11.data import types
from a11.status import Status, StatusCode, StatusException
from a11.stores.local_chunk_store import LocalChunkStore
from a11.stores.sqlite_chunk_store import SQLiteChunkStoreFactory


@pytest.fixture(params=["local", "sqlite"])
def make_store(request, tmp_path):
    """Build a store from each backend so the contract is tested on both.

    Everything below exercises the ChunkStore contract itself, not a backend
    detail, so both implementations must satisfy it identically. The SQLite
    stores share one root per test, which also covers many nodes living in a
    single database.
    """
    if request.param == "local":
        return LocalChunkStore
    factory = SQLiteChunkStoreFactory(str(tmp_path))
    return factory.open


def _deadline() -> timing.Time:
    return timing.now() + timing.Duration.seconds(2)


def _short_deadline() -> timing.Time:
    return timing.now() + timing.Duration.milliseconds(20)


def _fragment(seq: int, *, final: bool = False) -> types.NodeFragment:
    return types.NodeFragment(
        data=types.Chunk(data=str(seq)), seq=seq, continued=not final
    )


def _implicit_fragment(
    value: int, *, final: bool = False
) -> types.NodeFragment:
    return types.NodeFragment(
        data=types.Chunk(data=str(value)), continued=not final
    )


@pytest.mark.asyncio
async def test_basic_ops_work(make_store):
    store = make_store("test")
    seqs = await store.put_many(
        [_fragment(2, final=True), _fragment(0), _fragment(1)]
    )

    batches = [await store.next(deadline=_deadline()) for _ in range(4)]

    assert seqs == [2, 0, 1]
    assert [
        [
            fragment.get_chunk().data if fragment is not None else None
            for fragment in batch
        ]
        for batch in batches
    ] == [[b"0"], [b"1"], [b"2", None], [None]]


@pytest.mark.asyncio
async def test_get_waits_for_an_out_of_order_fragment(make_store):
    store = make_store("test")
    waiting = asyncio.create_task(store.get(1, deadline=_deadline()))
    await asyncio.sleep(0)

    await store.put(_fragment(0))
    assert not waiting.done()

    await store.put(_fragment(1, final=True))
    fragment = await waiting
    assert fragment.seq == 1
    assert fragment.get_chunk().data == b"1"


@pytest.mark.asyncio
async def test_multiple_independent_consumers(make_store):
    store = make_store("test")

    async def consume() -> list[bytes]:
        return [
            (await store.get(seq, deadline=_deadline())).get_chunk().data
            for seq in range(4)
        ]

    consumers = [asyncio.create_task(consume()) for _ in range(3)]
    await asyncio.sleep(0)
    for seq in (2, 0, 3, 1):
        await store.put(_fragment(seq, final=seq == 3))
        await asyncio.sleep(0)

    assert await asyncio.gather(*consumers) == [[b"0", b"1", b"2", b"3"]] * 3


@pytest.mark.asyncio
async def test_spmc(make_store):
    store = make_store("test")

    async def consume() -> list[int]:
        sequences = []
        while True:
            for fragment in await store.next(deadline=_deadline()):
                if fragment is None:
                    return sequences
                sequences.append(fragment.seq)

    consumers = [asyncio.create_task(consume()) for _ in range(3)]
    await asyncio.sleep(0)
    for seq in range(9):
        await store.put(_fragment(seq, final=seq == 8))
        await asyncio.sleep(0)

    consumed = await asyncio.gather(*consumers)
    assert sorted(seq for sequences in consumed for seq in sequences) == list(
        range(9)
    )


@pytest.mark.asyncio
async def test_mpmc(make_store):
    store = make_store("test")

    async def produce(sequences: tuple[int, ...]) -> None:
        for seq in sequences:
            await asyncio.sleep(0)
            await store.put(_fragment(seq, final=seq == 11))

    async def consume(arrival_order: int) -> int:
        fragment = await store.get_by_arrival_order(
            arrival_order, deadline=_deadline()
        )
        return fragment.seq

    consumers = [asyncio.create_task(consume(order)) for order in range(12)]
    producers = [
        asyncio.create_task(produce((0, 3, 6, 9))),
        asyncio.create_task(produce((1, 4, 7, 10))),
        asyncio.create_task(produce((2, 5, 8, 11))),
    ]

    await asyncio.gather(*producers)
    sequences_by_arrival = await asyncio.gather(*consumers)
    assert sorted(sequences_by_arrival) == list(range(12))
    assert len(set(sequences_by_arrival)) == 12


@pytest.mark.asyncio
@pytest.mark.parametrize("seed", range(10))
async def test_randomised_get_consumers_with_multiple_producers(
    seed: int, make_store
):
    rng = random.Random(seed)
    count = rng.randint(12, 30)
    producer_count = rng.randint(2, 5)
    store = make_store("randomised")
    sequences = list(range(count))
    rng.shuffle(sequences)
    batches = [
        sequences[index::producer_count] for index in range(producer_count)
    ]

    async def produce(batch: list[int]) -> None:
        for seq in batch:
            await asyncio.sleep(rng.random() / 10_000)
            await store.put(_fragment(seq, final=seq == count - 1))

    async def consume(order: list[int]) -> list[int]:
        result = []
        for seq in order:
            fragment = await store.get(seq, deadline=_deadline())
            result.append(fragment.seq)
            await asyncio.sleep(rng.random() / 10_000)
        return result

    consumer_orders = []
    for _ in range(rng.randint(2, 5)):
        order = list(range(count))
        rng.shuffle(order)
        consumer_orders.append(order)

    consumers = [
        asyncio.create_task(consume(order)) for order in consumer_orders
    ]
    producers = [asyncio.create_task(produce(batch)) for batch in batches]

    await asyncio.gather(*producers)
    assert await asyncio.gather(*consumers) == consumer_orders


@pytest.mark.asyncio
@pytest.mark.parametrize("seed", range(10))
async def test_randomised_next_consumers_receive_every_fragment_once(
    seed: int, make_store
):
    rng = random.Random(seed)
    count = rng.randint(12, 30)
    store = make_store("randomised")

    async def produce() -> None:
        pending = list(range(count))
        rng.shuffle(pending)
        for seq in pending:
            await asyncio.sleep(rng.random() / 10_000)
            await store.put(_fragment(seq, final=seq == count - 1))

    async def consume() -> list[int]:
        result = []
        while True:
            batch = await store.next(deadline=_deadline())
            result.extend(fragment.seq for fragment in batch if fragment)
            if batch[-1] is None:
                return result
            await asyncio.sleep(rng.random() / 10_000)

    consumers = [asyncio.create_task(consume()) for _ in range(4)]
    await produce()
    consumed = await asyncio.gather(*consumers)

    assert sorted(seq for result in consumed for seq in result) == list(
        range(count)
    )


@pytest.mark.asyncio
async def test_arrival_order_is_independent_of_sequence_order(make_store):
    store = make_store("test")
    await store.put_many([_fragment(4, final=True), _fragment(1), _fragment(3)])

    fragments = [
        await store.get_by_arrival_order(order, deadline=_deadline())
        for order in range(3)
    ]

    assert [fragment.seq for fragment in fragments] == [4, 1, 3]
    assert [
        await store.get_seq_for_arrival_order(order) for order in range(3)
    ] == [
        4,
        1,
        3,
    ]


@pytest.mark.asyncio
async def test_waiting_gets_time_out_with_deadline_exceeded(make_store):
    store = make_store("test")

    for make_waiting_call in (
        lambda: store.get(0, deadline=_short_deadline()),
        lambda: store.get_by_arrival_order(0, deadline=_short_deadline()),
        lambda: store.next(deadline=_short_deadline()),
    ):
        with pytest.raises(StatusException) as raised:
            await make_waiting_call()
        assert raised.value.status.code == StatusCode.DEADLINE_EXCEEDED


@pytest.mark.asyncio
async def test_elapsed_deadlines_are_reported_as_deadline_exceeded(make_store):
    store = make_store("test")
    elapsed = timing.now() - timing.Duration.milliseconds(1)

    for make_waiting_call in (
        lambda: store.get(0, deadline=elapsed),
        lambda: store.get_by_arrival_order(0, deadline=elapsed),
        lambda: store.next(deadline=elapsed),
    ):
        with pytest.raises(StatusException) as raised:
            await make_waiting_call()
        assert raised.value.status.code == StatusCode.DEADLINE_EXCEEDED


@pytest.mark.asyncio
async def test_next_returns_immediately_when_limit_is_reached(make_store):
    store = make_store("test")
    await store.put_many([_fragment(0), _fragment(1)])

    batch = await store.next(deadline=_deadline(), limit=2)

    assert [fragment.seq for fragment in batch] == [0, 1]


@pytest.mark.asyncio
async def test_next_returns_a_partial_batch_at_the_deadline(make_store):
    store = make_store("test")
    await store.put(_fragment(0))

    batch = await store.next(deadline=_short_deadline(), limit=2)

    assert [fragment.seq for fragment in batch] == [0]


@pytest.mark.asyncio
async def test_successful_close_appends_none_to_a_partial_batch(make_store):
    store = make_store("test")
    waiting = asyncio.create_task(store.next(deadline=_deadline(), limit=2))
    await asyncio.sleep(0)
    await store.put(_fragment(0))
    await store.close_writes_with_status(Status())

    batch = await waiting

    assert [fragment.seq if fragment else None for fragment in batch] == [
        0,
        None,
    ]


@pytest.mark.asyncio
async def test_error_close_returns_partial_batch_before_raising(make_store):
    store = make_store("test")
    waiting = asyncio.create_task(store.next(deadline=_deadline(), limit=2))
    await asyncio.sleep(0)
    await store.put(_fragment(0))
    await store.close_writes_with_status(
        Status(code=StatusCode.ABORTED, message="producer failed")
    )

    batch = await waiting
    assert [fragment.seq for fragment in batch] == [0]

    with pytest.raises(StatusException) as raised:
        await store.next(deadline=_deadline(), limit=2)
    assert raised.value.status.code == StatusCode.ABORTED


@pytest.mark.asyncio
async def test_error_close_wakes_all_kinds_of_waiter(make_store):
    store = make_store("test")
    waiters = [
        asyncio.create_task(store.get(3, deadline=_deadline())),
        asyncio.create_task(
            store.get_by_arrival_order(3, deadline=_deadline())
        ),
        asyncio.create_task(store.next(deadline=_deadline())),
    ]
    await asyncio.sleep(0)
    await store.close_writes_with_status(
        Status(code=StatusCode.ABORTED, message="producer failed")
    )

    results = await asyncio.gather(*waiters, return_exceptions=True)
    assert all(isinstance(result, StatusException) for result in results)
    assert [result.status.code for result in results] == [
        StatusCode.ABORTED
    ] * 3


@pytest.mark.asyncio
async def test_clear_data_returns_original_and_leaves_a_tombstone(make_store):
    store = make_store("test")
    await store.put(_fragment(0, final=True))

    cleared = await store.clear_data(0)
    stored = await store.get(0, deadline=_deadline())

    assert cleared.get_chunk().data == b"0"
    assert stored.get_chunk().data == b""
    assert stored.get_chunk().ref == "__tombstone__"
    assert await store.size() == 1


@pytest.mark.asyncio
async def test_put_many_validation_is_atomic(make_store):
    store = make_store("test")

    with pytest.raises(StatusException) as raised:
        await store.put_many([_fragment(0), _fragment(0, final=True)])

    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
    assert await store.size() == 0
    assert await store.get_final_seq() is None


@pytest.mark.asyncio
async def test_put_returns_explicit_and_implicit_sequences(make_store):
    store = make_store("test")

    assert await store.put(_fragment(4)) == 4
    assert await store.put(_implicit_fragment(1)) == 1


@pytest.mark.asyncio
async def test_put_many_assigns_and_returns_implicit_sequences(make_store):
    store = make_store("test")

    seqs = await store.put_many(
        [
            _implicit_fragment(0),
            _implicit_fragment(1),
            _implicit_fragment(2, final=True),
        ]
    )

    assert seqs == [0, 1, 2]
    assert [
        (await store.get(seq, deadline=_deadline())).get_chunk().data
        for seq in seqs
    ] == [b"0", b"1", b"2"]


@pytest.mark.asyncio
async def test_put_many_rejects_mixed_explicit_and_implicit_sequences(
    make_store,
):
    store = make_store("test")

    with pytest.raises(StatusException) as raised:
        await store.put_many([_implicit_fragment(0), _fragment(1)])

    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
    assert await store.size() == 0


@pytest.mark.asyncio
async def test_put_many_collision_does_not_change_implicit_assignment(
    make_store,
):
    store = make_store("test")
    await store.put(_fragment(0))

    with pytest.raises(StatusException) as raised:
        await store.put_many([_fragment(1), _fragment(0)])

    assert raised.value.status.code == StatusCode.ALREADY_EXISTS
    assert await store.size() == 1
    assert await store.put(_implicit_fragment(1)) == 1


@pytest.mark.asyncio
async def test_put_many_late_validation_failure_is_atomic(make_store):
    """A rule checked after sequences are assigned still writes nothing.

    Two final fragments are only detectable once the batch has been laid out,
    so this exercises the rollback path rather than the cheap pre-checks.
    """
    store = make_store("test")

    with pytest.raises(StatusException) as raised:
        await store.put_many(
            [_fragment(0), _fragment(1, final=True), _fragment(2, final=True)]
        )

    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
    assert await store.size() == 0
    assert await store.put(_implicit_fragment(0)) == 0


@pytest.mark.asyncio
async def test_local_store_rejects_node_ref_payloads():
    """NodeRef support is a backend property, not part of the contract.

    The in-memory store has nowhere to resolve a reference, so it refuses one.
    `SQLiteChunkStore` accepts them and stores the target as indexed columns;
    see `test_sqlite_chunk_store.py`.
    """
    store = LocalChunkStore("test")
    unsupported = types.NodeFragment(
        seq=1,
        data=types.NodeRef.model_construct(
            id="another-node", offset=0, length=None
        ),
        continued=True,
    )

    with pytest.raises(StatusException) as raised:
        await store.put_many([_fragment(0), unsupported])

    assert raised.value.status.code == StatusCode.UNIMPLEMENTED
    assert await store.size() == 0


@pytest.mark.asyncio
async def test_concurrent_implicit_puts_receive_unique_sequences(make_store):
    store = make_store("test")

    seqs = await asyncio.gather(
        *(store.put(_implicit_fragment(value)) for value in range(10))
    )

    assert sorted(seqs) == list(range(10))
    assert await store.size() == 10


@pytest.mark.asyncio
async def test_explicit_final_sequence_is_reported(make_store):
    store = make_store("test")
    await store.put_many([_fragment(1, final=True), _fragment(0)])

    assert await store.get_final_seq() == 1
    first = await store.get(0, deadline=_deadline())
    final = await store.get(1, deadline=_deadline())
    assert first.continued is True
    assert final.continued is False


@pytest.mark.asyncio
async def test_implicit_sequences_support_a_final_fragment(make_store):
    store = make_store("test")
    await store.put_many(
        [
            types.NodeFragment(data=types.Chunk(data="0"), continued=True),
            types.NodeFragment(data=types.Chunk(data="1"), continued=False),
        ]
    )

    assert await store.get_final_seq() == 1
    first = await store.next(deadline=_deadline())
    final = await store.next(deadline=_deadline())
    after_final = await store.next(deadline=_deadline())

    assert [fragment.seq for fragment in first] == [0]
    assert [fragment.seq if fragment else None for fragment in final] == [
        1,
        None,
    ]
    assert after_final == [None]


@pytest.mark.asyncio
async def test_put_is_rejected_after_writes_are_closed(make_store):
    store = make_store("test")
    await store.close_writes_with_status(Status())

    with pytest.raises(StatusException) as raised:
        await store.put(_fragment(0, final=True))
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION


@pytest.mark.asyncio
async def test_ok_close_wakes_next_waiter_and_ends_iteration(make_store):
    store = make_store("test")
    waiting = asyncio.create_task(store.next(deadline=_deadline()))
    await asyncio.sleep(0)

    await store.close_writes_with_status(Status())

    assert await waiting == [None]
