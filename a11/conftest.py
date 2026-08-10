import os

import pytest

import a11


@pytest.fixture(scope="session", autouse=True)
def init_logging():
    os.environ["A11_DEBUG"] = "1"
    a11.enable_logging("debug")
    a11.get_logger(__name__).info("Logging initialized")
