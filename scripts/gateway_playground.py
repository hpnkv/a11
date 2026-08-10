import asyncio
from typing import Sequence

import a11
from a11 import StatusCode
from a11.gateway.app import PING_SCHEMA
from a11.sdk import audio
from a11.sdk.audio.actions import TRANSCRIBE_AUDIO
from absl import app
from absl import logging

from a11.status import StatusException


async def main(_: Sequence[str]):
    options = a11.WebSocketClientOptions()
    options.http2_options.client_preference = a11.HttpProtocolPreference.HTTP11
    stream = a11.WebSocketWireStream.connect(
        f"ws://127.0.0.1:8011/a11",
        websocket_options=options,
    )

    session = a11.Session()
    await session.add_stream(stream, mode="start")

    ping = (
        a11.Action(PING_SCHEMA)
        .bind_node_map(session.node_map)
        .bind_stream(stream)
        .bind_session(session)
    )
    await ping.call()
    async with ping["input"]:
        await ping["input"].put_final("ping")

    logging.info("here")

    output_value = await ping["output"].consume(str)
    if output_value == "ping":
        logging.info("ping successful")
    await ping.wait()

    audio_registry = a11.ActionRegistry()
    audio.actions.register(audio_registry)

    # capture_audio_schema = audio_registry.get_schema(
    #     audio.actions.CAPTURE_AUDIO
    # )
    # capture_audio = (
    #     a11.Action(capture_audio_schema)
    #     .bind_node_map(session.node_map)
    #     .bind_stream(stream)
    #     .bind_session(session)
    # )
    # await capture_audio.call()
    # async with (capture_audio["options"] as capture_options,):
    #     await capture_options.put_final(
    #         audio.AudioInputOptions(device_name="MacBook Pro Microphone")
    #     )
    # logging.info("capture_audio called")
    #
    # timeout = 60.0
    # deadline = a11.now() + a11.Duration.seconds(timeout)
    # while True:
    #     try:
    #         buffer = await capture_audio["audio"].next(
    #             timeout=max(a11.zero_duration(), deadline - a11.now())
    #         )
    #     except StatusException as exc:
    #         if exc.status.code == StatusCode.DEADLINE_EXCEEDED:
    #             logging.info("deadline exceeded")
    #             await capture_audio["control_events"].put(
    #                 audio.actions.AudioControlEvent.stop()
    #             )
    #             logging.info("stop event sent")
    #         raise
    #
    #     if buffer is None:
    #         logging.info("audio stream closed")
    #         break
    #
    #     print(
    #         buffer.num_channels,
    #         buffer.num_frames,
    #         buffer.sample_rate,
    #         buffer.end_time,
    #     )
    #
    # await capture_audio["control_events"].put_null_final()
    # await capture_audio["control_events"].drain_and_close()

    capture_transcription_schema = audio_registry.get_schema(
        audio.actions.CAPTURE_TRANSCRIPTION
    )

    capture_transcription = (
        a11.Action(capture_transcription_schema)
        .bind_node_map(session.node_map)
        .bind_stream(stream)
        .bind_session(session)
    )
    await capture_transcription.call()
    async with (
        capture_transcription["capture_options"] as capture_options,
        capture_transcription["asr_options"] as asr_options_node,
    ):
        await capture_options.put_final(
            audio.AudioInputOptions(device_name="MacBook Pro Microphone")
        )
        await asr_options_node.put_final(
            audio.SpeechRecognizerOptions(
                model_path="/Users/helena/.cache/a11/audio/ggml-tiny.en.bin",
                vad_model_path=(
                    "/Users/helena/.cache/a11/audio/ggml-silero-v5.1.2.bin"
                ),
            )
        )
    logging.info("capture_transcription called")

    timeout = 60.0
    deadline = a11.now() + a11.Duration.seconds(timeout)
    while True:
        try:
            piece = await capture_transcription["transcription_pieces"].next(
                timeout=max(a11.zero_duration(), deadline - a11.now())
            )
            logging.info("piece: %s", piece)
        except StatusException as exc:
            if exc.status.code == StatusCode.DEADLINE_EXCEEDED:
                logging.info("deadline exceeded")
                await capture_transcription["control_events"].put(
                    audio.actions.AudioControlEvent.stop()
                )
                logging.info("stop event sent")
            raise

        if piece is None:
            logging.info("transcription_pieces stream closed")
            break

    await capture_transcription["control_events"].put_null_final()
    await capture_transcription["control_events"].drain_and_close()

    session.half_close()
    try:
        await asyncio.wait_for(session.done.wait(), timeout=1.0)
    except asyncio.TimeoutError:
        logging.warning("session was not done on exit")


def sync_main(argv: Sequence[str]):
    asyncio.run(main(argv))


if __name__ == "__main__":
    app.run(sync_main)
