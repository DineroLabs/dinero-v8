import pytest

from _w5_load import load

rt = load("rpc_transcript")

TXID = "ab" * 32
OTHER = "cd" * 32
ADDR = "rdin1pq4gtgwl6hrnp3n897tj9gv69vykep4cfa5xmyt2e9py2v8n9q38qgnp2z3"


def norm(ports=(), pids=()):
    return rt.Normalizer("/tmp/sep_w5_x", ports, pids)


def test_integer_equal_to_a_port_or_pid_is_not_masked_outside_port_pid_keys():
    n = norm(ports={49566}, pids={1234})
    v = {"bytes_written": 49566, "amount_una": 1234, "coins": [49566, 1234]}
    assert n.value(v) == v


def test_port_and_pid_keys_are_masked_with_type_tag():
    n = norm(ports={49566}, pids={1234})
    assert n.value({"port": 49566, "pid": 1234, "rpcport": 7}) == {
        "port": "<PORT:int>", "pid": "<PID:int>", "rpcport": 7}


def test_port_inside_string_is_masked():
    assert norm(ports={49566}).value("127.0.0.1:49566") == "127.0.0.1:<PORT>"
    assert norm(ports={49566}).value("127.0.0.1:495660") == "127.0.0.1:495660"


def test_hex_token_inside_string_and_repeated_txid_share_one_ordinal():
    n = norm()
    out = n.value({"txid": TXID, "message": "tx %s rejected" % TXID,
                   "prev": [OTHER, TXID]})
    # keys walk sorted: message, prev, txid
    assert out == {"message": "tx <H#1> rejected", "prev": ["<H#2>", "<H#1>"],
                   "txid": "<H#1>"}


def test_ordinals_persist_across_entries_and_are_case_insensitive():
    n = norm()
    assert n.value(TXID) == "<H#1>"
    assert n.value(OTHER) == "<H#2>"
    assert n.value(TXID.upper()) == "<H#1>"


def test_regtest_address_is_normalized_but_non_bech32_text_is_not():
    n = norm()
    assert n.value({"address": ADDR, "m": "pay " + ADDR}) == {"address": "<A#1>", "m": "pay <A#1>"}
    assert n.value("rdin1notanaddress") == "rdin1notanaddress"


def test_time_fields_keep_their_json_type():
    n = norm()
    assert n.value({"time": 5}) == {"time": "<T:int>"}
    assert n.value({"time": 5.0}) == {"time": "<T:float>"}
    assert n.value({"time": "2026-10-29T22:22:26Z"}) == {"time": "<T:str>"}
    assert n.value({"eta": 0}) == {"eta": "<D:int>"}
    assert n.value({"eta": 0.0}) == {"eta": "<D:float>"}
    assert n.value({"time": 5}) != n.value({"time": 5.0})


def test_int_float_bool_null_are_not_conflated():
    n = norm()
    v = {"a": 0, "b": 0.0, "c": False, "d": None}
    out = n.value(v)
    assert out == v
    assert [type(out[k]) for k in "abcd"] == [int, float, bool, type(None)]


def test_hex_blob_rule_keeps_length_and_equality_only_above_128_chars():
    n = norm()
    spk = "5120" + "ab" * 32           # 68 hex: compared raw
    k128 = "ef" * 64                   # 128 hex: compared raw
    blob = "01" * 65                   # 130 hex: blob
    assert n.value(spk) == spk
    assert n.value(k128) == k128
    assert n.value(blob) == "<X#1:len=130>"
    assert n.value({"pubkey_hex": blob, "sig": "02" * 70}) == {
        "pubkey_hex": "<X#1:len=130>", "sig": "<X#2:len=140>"}


def test_field_names_never_rewritten_but_value_keys_are():
    n = norm()
    out = n.value({"txid": TXID, ADDR: 1.5, "error": {"code": -32603, "message": "Insufficient funds"}})
    assert out == {"txid": "<H#1>", "<A#1>": 1.5,
                   "error": {"code": -32603, "message": "Insufficient funds"}}


def test_work_dir_is_masked_in_strings():
    assert norm().value("/tmp/sep_w5_x/snapshot.dat") == "<WORK>/snapshot.dat"
