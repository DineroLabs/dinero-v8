"""Tests for chain_read_sites.py (spec section 7.4, plan Task 0.2)."""
import json
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "chain_read_sites.py"

BASELINE_CPP = """\
#include "x.h"
class ChainDBUTXOView : public UTXOView {
    bool HaveUTXO() const override {
        auto c = chain_db_->getCoinWithConfidentialFallback(txid, vout);
        return c.ok();
    }
};

TxAcceptResult Mempool::submitTransactionInternal(const Transaction& tx) {
    auto guard = chainstate_read_guard_factory_ ? chainstate_read_guard_factory_() : nullptr;
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto tip = chain_db_->getTip();
    auto again = chain_db_->getTip();
    return {};
}

bool Mempool::submitTransactionTestOnly(const Transaction& tx) {
    auto tip = chain_db_->getTip();
    chain_db_->forEachUTXO([&](auto&&...) { return true; });
    return true;
}
"""


def run(*args, cwd=None):
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          capture_output=True, text=True, cwd=cwd)


def record(tmp_path, text, name="mempool.cpp"):
    src = tmp_path / name
    src.write_text(text)
    out = tmp_path / (name + ".json")
    r = run("record", str(src), "--out", str(out))
    assert r.returncode == 0, r.stderr
    return src, out, json.loads(out.read_text())


def test_records_reads_per_function(tmp_path):
    _, _, data = record(tmp_path, BASELINE_CPP)
    fns = data["functions"]
    assert fns["ChainDBUTXOView"]["reads"] == {"getCoin": 1}
    sub = fns["Mempool::submitTransactionInternal"]
    assert sub["reads"] == {"getTip": 2}
    assert sub["guards"] == 1 and sub["guards_before_pool_lock"] == [True]
    test_only = fns["Mempool::submitTransactionTestOnly"]
    assert test_only["reads"] == {"getTip": 1, "commitmentScan": 1}
    assert test_only["guards"] == 0
    assert data["total_reads"] == 5


def test_view_call_maps_to_same_read(tmp_path):
    _, base, _ = record(tmp_path, BASELINE_CPP)
    after = (BASELINE_CPP
             .replace("chain_db_->getCoinWithConfidentialFallback", "chain_view_->GetCoinWithConfidentialFallback")
             .replace("chain_db_->getTip()", "chain_view_->GetTip()")
             .replace("chain_db_->forEachUTXO([&](auto&&...) { return true; })",
                      "chain_view_->HaveConfidentialCommitment(commitment)"))
    src = tmp_path / "after.cpp"
    src.write_text(after)
    r = run("check", str(src), "--baseline", str(base))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "DIFF" not in r.stdout


def test_renamed_storage_free_adapter_keeps_read_identity(tmp_path):
    _, base, _ = record(tmp_path, BASELINE_CPP)
    after = BASELINE_CPP.replace("ChainDBUTXOView", "MempoolChainUTXOView")
    after = after.replace("chain_db_->getCoinWithConfidentialFallback",
                          "chain_view_->GetCoinWithConfidentialFallback")
    src = tmp_path / "renamed.cpp"
    src.write_text(after)
    result = run("check", str(src), "--baseline", str(base))
    assert result.returncode == 0, result.stdout + result.stderr


def test_moved_guard_is_diff(tmp_path):
    _, base, _ = record(tmp_path, BASELINE_CPP)
    moved = BASELINE_CPP.replace(
        "    auto guard = chainstate_read_guard_factory_ ? chainstate_read_guard_factory_() : nullptr;\n"
        "    std::unique_lock<std::shared_mutex> lock(m_mutex);\n",
        "    std::unique_lock<std::shared_mutex> lock(m_mutex);\n"
        "    auto guard = chainstate_read_guard_factory_ ? chainstate_read_guard_factory_() : nullptr;\n")
    assert moved != BASELINE_CPP
    src = tmp_path / "moved.cpp"
    src.write_text(moved)
    r = run("check", str(src), "--baseline", str(base))
    assert r.returncode == 1
    assert "DIFF Mempool::submitTransactionInternal" in r.stdout


def test_added_read_is_diff(tmp_path):
    _, base, _ = record(tmp_path, BASELINE_CPP)
    added = BASELINE_CPP.replace(
        "    auto tip = chain_db_->getTip();\n    chain_db_->forEachUTXO",
        "    auto tip = chain_db_->getTip();\n    auto t2 = chain_db_->getTip();\n    chain_db_->forEachUTXO")
    assert added != BASELINE_CPP
    src = tmp_path / "added.cpp"
    src.write_text(added)
    r = run("check", str(src), "--baseline", str(base))
    assert r.returncode == 1
    assert "DIFF Mempool::submitTransactionTestOnly" in r.stdout


def test_adapter_mutex_rejected(tmp_path):
    _, base, _ = record(tmp_path, BASELINE_CPP)
    adapter = tmp_path / "chain_db_mempool_view.h"
    adapter.write_text("class ChainDbMempoolView {\n"
                       "  StatusOr<Tip> GetTip() const { std::lock_guard<std::mutex> g(m_); return db_->getTip(); }\n"
                       "};\n")
    r = run("check", str(tmp_path / "mempool.cpp"), "--baseline", str(base), "--adapter", str(adapter))
    assert r.returncode == 1
    assert "ADAPTER_LOCK" in r.stdout
    clean = tmp_path / "clean_view.h"
    clean.write_text("class ChainDbMempoolView {\n"
                     "  StatusOr<Tip> GetTip() const { return db_->getTip(); }\n"
                     "};\n")
    r = run("check", str(tmp_path / "mempool.cpp"), "--baseline", str(base), "--adapter", str(clean))
    assert r.returncode == 0, r.stdout


# --- Review findings (independent review of Task 0.2): each must FAIL on the
# first implementation and PASS after the fix. ---------------------------------

def _inv(tmp_path, text, name):
    _, _, data = record(tmp_path, text, name)
    return data


GUARD_FN = """\
TxAcceptResult Mempool::f() {
    auto g = chainstate_read_guard_factory_();
    %s
    return {};
}
"""


def test_lock_spellings_without_template_args_are_locks(tmp_path):
    for i, lock in enumerate(["std::unique_lock lk(m_mutex);",
                              "std::scoped_lock lk{m_mutex};",
                              "m_mutex.lock();",
                              "std::shared_lock<std::shared_mutex> lk(\n        m_mutex);"]):
        before = _inv(tmp_path, GUARD_FN % lock, "b%d.cpp" % i)
        assert before["functions"]["Mempool::f"]["guards_before_pool_lock"] == [True], lock
        moved = ("TxAcceptResult Mempool::f() {\n    %s\n"
                 "    auto g = chainstate_read_guard_factory_();\n    return {};\n}\n") % lock
        after = _inv(tmp_path, moved, "a%d.cpp" % i)
        assert after["functions"]["Mempool::f"]["guards_before_pool_lock"] == [False], lock


def test_guard_without_visible_lock_is_unresolved(tmp_path):
    data = _inv(tmp_path, GUARD_FN % "helper_lock();", "u.cpp")
    assert data["functions"]["Mempool::f"]["guards_before_pool_lock"] == [None]
    src = tmp_path / "u.cpp"
    r = run("check", str(src), "--baseline", str(tmp_path / "u.cpp.json"))
    assert r.returncode == 1 and "UNRESOLVED Mempool::f" in r.stdout


def test_wrapped_and_spaced_reads_are_counted(tmp_path):
    text = ("void Mempool::f() {\n"
            "    auto a = chain_view_\n        ->GetTip();\n"
            "    auto b = chain_db_ -> getTip();\n"
            "    auto c = chain_db_fallback_\n            ->getCoin(o);\n"
            "}\n")
    data = _inv(tmp_path, text, "w.cpp")
    assert data["functions"]["Mempool::f"]["reads"] == {"fallbackGetCoin": 1, "getTip": 2}


def test_read_through_alias_is_not_silently_dropped(tmp_path):
    # Pin (fail-safe already before the fix): an aliased read must never compare equal.
    base = _inv(tmp_path, "void Mempool::f() {\n    auto t = chain_db_->getTip();\n}\n", "x.cpp")
    alias = "void Mempool::f() {\n    auto& v = *chain_view_;\n    auto t = v.GetTip();\n}\n"
    src = tmp_path / "alias.cpp"
    src.write_text(alias)
    r = run("check", str(src), "--baseline", str(tmp_path / "x.cpp.json"))
    assert r.returncode == 1, r.stdout   # must not pass as identical
    assert base["functions"]["Mempool::f"]["reads"] == {"getTip": 1}


def test_block_comment_read_is_not_counted(tmp_path):
    text = ("void Mempool::f() {\n"
            "    /* chain_db_->getTip(); */\n"
            "    auto t = chain_db_->getCoinWithConfidentialFallback(a, b);\n"
            "}\n")
    data = _inv(tmp_path, text, "c1.cpp")
    assert data["functions"]["Mempool::f"]["reads"] == {"getCoin": 1}


def test_slashes_in_string_do_not_hide_code(tmp_path):
    text = ("void Mempool::f() {\n"
            "    log(\"http://x\"); auto t = chain_db_->getTip();\n"
            "    // chain_db_->getTip();\n"
            "}\n")
    data = _inv(tmp_path, text, "c2.cpp")
    assert data["functions"]["Mempool::f"]["reads"] == {"getTip": 1}


def test_split_signature_and_free_helpers_are_own_functions(tmp_path):
    text = ("void Mempool::a() {\n    auto t = chain_db_->getTip();\n}\n"
            "static void\nhelper(const ChainDB* chain_db_) {\n"
            "    auto t = chain_db_->getTip();\n"
            "    auto g = chainstate_read_guard_factory_();\n}\n"
            "bool\nMempool::b(int x,\n               int y) {\n"
            "    auto t = chain_db_->getTip();\n}\n"
            "class V : public X {\n  int h() const { return chain_db_->getTip().ok(); }\n};\n"
            "void free_after_class() { auto t = chain_db_->getTip(); }\n")
    fns = _inv(tmp_path, text, "s.cpp")["functions"]
    assert fns["Mempool::a"]["reads"] == {"getTip": 1}
    assert fns["helper"]["reads"] == {"getTip": 1} and fns["helper"]["guards"] == 1
    assert fns["Mempool::b"]["reads"] == {"getTip": 1}
    assert fns["V"]["reads"] == {"getTip": 1}
    assert fns["free_after_class"]["reads"] == {"getTip": 1}
