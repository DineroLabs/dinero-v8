"""The MP4 file decoder must refuse incomplete or altered persistence data."""
import importlib.util
import struct
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[4]
SCRIPT = ROOT / "tests/mempool_separation/mempool_transcript.py"
spec = importlib.util.spec_from_file_location("mempool_transcript", SCRIPT)
transcript = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transcript)


def file_bytes(body=b"tx"):
    # One entry: magic, version, count, size, transaction, arrival, fee, height.
    return b"MEMPOOLV" + struct.pack("<I", 1) + bytes([1, len(body)]) + body + \
        struct.pack("<QQI", 100, 200, 300)


def test_decoder_keeps_format_and_entry_fields(tmp_path):
    path = tmp_path / "mempool.dat"
    path.write_bytes(file_bytes())
    assert transcript.persisted(path) == {
        "magic": "MEMPOOLV", "version": 1,
        "entries": [{"transaction_hex": "7478", "arrival_time": "<T:int>",
                     "fee": 200, "height": 300}]}


@pytest.mark.parametrize("altered", [b"", b"BADPOOLV" + file_bytes()[8:],
                                    file_bytes()[:-1], file_bytes() + b"x"])
def test_decoder_refuses_corrupt_or_partial_file(tmp_path, altered):
    path = tmp_path / "mempool.dat"
    path.write_bytes(altered)
    with pytest.raises(transcript.rpc.RunError):
        transcript.persisted(path)


def test_saved_body_must_match_submitted_bytes_even_at_same_length(tmp_path):
    path = tmp_path / "mempool.dat"
    path.write_bytes(file_bytes(b"tx"))
    state = transcript.persisted(path)
    transcript.expect_persisted_transaction(state, "7478")
    with pytest.raises(transcript.rpc.RunError, match="exact submitted transaction bytes"):
        transcript.expect_persisted_transaction(state, "7479")
