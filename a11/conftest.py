import os

from absl import logging

logging._warn_preinit_stderr = False

import pytest


@pytest.fixture(scope="session", autouse=True)
def init_logging():
    os.environ["A11_DEBUG"] = "1"
    logging.use_absl_handler()
    logging.set_verbosity(logging.DEBUG)
    logging.info("Logging initialized")
