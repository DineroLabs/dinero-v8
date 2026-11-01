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
