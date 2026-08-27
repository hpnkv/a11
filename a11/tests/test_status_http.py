# Copyright 2026 The A11 Authors.

"""`Status` at an HTTP boundary, which is where two silent failures lived.

A `Status` is the native class with a pydantic core schema attached, not a
`BaseModel`, and FastAPI has two paths that assumed otherwise:

* `jsonable_encoder` fell through every branch it knows and ended at
  `vars(obj)`, which for a pybind11 object is empty -- so a service answering
  its errors as statuses sent `{}` and nothing complained;
* the OpenAPI document is itself serialised by pydantic, so a `Status` used as
  a response *example* made `app.openapi()` raise.

Both are fixed in `a11.status`, and both are the kind of thing that only shows
up in a running service, so they are pinned here.
"""

from fastapi import FastAPI
from fastapi.encoders import jsonable_encoder
from fastapi.responses import JSONResponse
from fastapi.testclient import TestClient

from a11.status import Status, StatusCode, StatusException


def test_jsonable_encoder_renders_a_status_in_full():
    status = Status(
        code=StatusCode.NOT_FOUND,
        message="no such identity",
        details=[{"identity": "absent"}],
    )

    assert jsonable_encoder(status) == {
        "code": 5,
        "message": "no such identity",
        "details": [{"identity": "absent"}],
    }


def test_jsonable_encoder_renders_a_status_nested_in_a_document():
    status = Status(code=StatusCode.UNAVAILABLE, message="try later")

    assert jsonable_encoder({"outcome": status})["outcome"]["code"] == 14


def test_documented_status_responses_produce_a_serialisable_schema():
    app = FastAPI()

    @app.get(
        "/thing",
        responses=Status.get_fastapi_response_dict_for_codes(
            StatusCode.NOT_FOUND, StatusCode.PERMISSION_DENIED
        ),
    )
    async def read_thing() -> dict:
        return {}

    document = app.openapi()

    responses = document["paths"]["/thing"]["get"]["responses"]
    assert set(responses) >= {"404", "403", "422"}
    example = responses["404"]["content"]["application/json"]["example"]
    # Keep the example as plain data so pydantic can serialize its schema.
    assert isinstance(example, dict)
    assert example["code"] == 5


def test_a_status_error_body_survives_the_round_trip():
    app = FastAPI()

    @app.exception_handler(StatusException)
    async def handle(_, exc: StatusException) -> JSONResponse:
        return JSONResponse(
            status_code=exc.status.code.to_http_code(),
            content=jsonable_encoder(exc.status),
        )

    @app.get("/fails")
    async def fails() -> dict:
        raise Status(
            code=StatusCode.PERMISSION_DENIED,
            message="not yours",
            details=[{"needs": "IDENTITY_HOST"}],
        ).to_exception()

    with TestClient(app, raise_server_exceptions=False) as client:
        response = client.get("/fails")

    assert response.status_code == 403
    # Structured details survive the HTTP boundary.
    assert response.json() == {
        "code": 7,
        "message": "not yours",
        "details": [{"needs": "IDENTITY_HOST"}],
    }
    parsed = Status.model_validate(response.json())
    assert parsed.code == StatusCode.PERMISSION_DENIED
