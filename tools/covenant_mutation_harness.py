#!/usr/bin/env python3
"""Mutation-coverage harness for the covenant consensus rules (CTV + CCV).

WHY THIS EXISTS
---------------
A green test suite proves the tests pass. It does not prove they would FAIL if
a consensus rule were removed. This harness answers that second question
mechanically: it deletes one consensus rule at a time from production code,
rebuilds, runs the covenant test lane, and requires that the lane goes red.

A mutation that survives -- production code broken, tests still green -- is a
coverage gap and is reported as a finding, not a warning.

This automates what was previously done by hand, one neuter at a time, and
turns those ad-hoc proofs into a standing, reproducible mutation score.

SAFETY
------
This harness edits production source by construction. Restoration is therefore
treated as the primary correctness property, not a convenience:

  * the original bytes and their SHA-256 are captured before any edit;
  * restoration runs in a finally block, so it happens on success, on failure,
    on exception, and on Ctrl-C;
  * after restoring, the digest is re-verified and the run aborts loudly if it
    does not match.

Never leave this harness's edits in a commit. If it aborts with a digest
mismatch, restore the file from git before doing anything else.

USAGE
-----
    python3 tools/covenant_mutation_harness.py --build-dir build
    python3 tools/covenant_mutation_harness.py --build-dir build --only ccv_value_preservation

Exit status is 0 only when every mutation is caught.
"""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent

# Test targets that must be rebuilt and run for each mutation. Keep this list
# aligned with the covenant lane; a mutation "caught" by a target that is not
# rebuilt would be a false pass.
TARGETS = [
    "test_bip119_ctv_vectors",
    "test_ccv_successor_binding",
    "test_ccv_adversarial",
    "test_ccv_reference_model",
    "test_covenant_activation",
]

CTEST_TESTS = [
    "BIP119CTVVectors",
    "CcvSuccessorBinding",
    "CcvAdversarial",
    "CcvReferenceModel",
    "CovenantActivation",
]

COVENANTS = "src/consensus/covenants.cpp"

# Each mutation removes exactly one consensus rule. `old` must appear exactly
# once in the file -- an ambiguous match aborts rather than guessing.
MUTATIONS = [
    # ---- CTV: BIP-119 template hash construction -----------------------------
    {
        "id": "ctv_scriptsig_length_prefix",
        "file": COVENANTS,
        "rule": "BIP119 scriptSig hash commits to CompactSize-prefixed scripts",
        "old": """            WriteCompactSize(scriptSigData, vin.scriptSig.size());
            scriptSigData.insert(scriptSigData.end(),""",
        "new": """            scriptSigData.insert(scriptSigData.end(),""",
    },
    {
        "id": "ctv_scriptsig_conditional",
        "file": COVENANTS,
        "rule": "scriptSig hash is included only when some scriptSig is non-empty",
        "old": "    if (hasNonEmptyScriptSig_) {\n        preimage.insert(",
        "new": "    if (true) {\n        preimage.insert(",
    },
    {
        "id": "ctv_input_index_binding",
        "file": COVENANTS,
        "rule": "the template hash commits to the spending input index",
        "old": "    WriteLE32(preimage, inputIndex);",
        "new": "    WriteLE32(preimage, 0);",
    },
    {
        "id": "ctv_output_length_prefix",
        "file": COVENANTS,
        "rule": "output hash commits to CompactSize-prefixed scriptPubKeys",
        "old": """        WriteLE64(outputData, vout.value.GetUna());
        WriteCompactSize(outputData, vout.scriptPubKey.size());""",
        "new": """        WriteLE64(outputData, vout.value.GetUna());""",
    },
    {
        "id": "ctv_rejects_dinero_extensions",
        "file": COVENANTS,
        "rule": "CTV refuses shielded / explicit-fee / confidential transactions",
        "old": """        !Transaction::IsShieldedVersion(tx.version) &&
        !tx.has_explicit_fee &&
        !tx.HasConfidentialOutputs() &&""",
        "new": "",
    },
    # ---- CCV: successor binding ---------------------------------------------
    {
        "id": "ccv_counter_increment",
        "file": COVENANTS,
        "rule": "next.counter == previous.counter + 1",
        "old": "        newState.counter != prevState.counter + 1 ||\n",
        "new": "",
    },
    {
        "id": "ccv_counter_terminal",
        "file": COVENANTS,
        "rule": "previous.counter != UINT32_MAX (no wrap)",
        "old": "    if (prevState.counter == UINT32_MAX ||\n",
        "new": "    if (false ||\n",
    },
    {
        "id": "ccv_code_immutability",
        "file": COVENANTS,
        "rule": "next.codeHash == previous.codeHash",
        "old": "        newState.codeHash != prevState.codeHash ||\n",
        "new": "",
    },
    {
        "id": "ccv_code_identity",
        "file": COVENANTS,
        "rule": "previous.codeHash == SHA256(revealed tapscript)",
        "old": "        ComputeContractCodeHash(spendContext.tapscript) != prevState.codeHash) {",
        "new": "        false) {",
    },
    {
        "id": "ccv_state_hash_recompute",
        "file": COVENANTS,
        "rule": "both state hashes recompute from their contents",
        "old": """        ComputeContractStateHash(prevState) != prevState.stateHash ||
        ComputeContractStateHash(newState) != newState.stateHash ||""",
        "new": "",
    },
    {
        "id": "ccv_value_preservation",
        "file": COVENANTS,
        "rule": "successor preserves the spent value exactly",
        "old": "        successor.value != spent.value ||\n",
        "new": "",
    },
    {
        "id": "ccv_transparent_spent",
        "file": COVENANTS,
        "rule": "the spent CCV output must be transparent",
        "old": """    const UTXOEntry& spent = spendContext.inputUtxos[inputIndex];
    if (spent.is_confidential) {
        return false;
    }""",
        "new": "    const UTXOEntry& spent = spendContext.inputUtxos[inputIndex];",
    },
    {
        "id": "ccv_transparent_successor",
        "file": COVENANTS,
        "rule": "the successor output must be transparent",
        "old": "    if (successor.is_confidential ||\n",
        "new": "    if (false ||\n",
    },
    {
        "id": "ccv_parity_binding",
        "file": COVENANTS,
        "rule": "control-block parity matches the derived output key",
        "old": "        expectedParity != spendContext.outputKeyParity ||\n",
        "new": "",
    },
    {
        "id": "ccv_state_size_limit",
        "file": COVENANTS,
        "rule": "state data is bounded by MAX_CONTRACT_STATE_DATA_SIZE",
        "old": """        prevState.data.size() > MAX_CONTRACT_STATE_DATA_SIZE ||
        newState.data.size() > MAX_CONTRACT_STATE_DATA_SIZE) {""",
        "new": "        false) {",
    },
    {
        "id": "ccv_unique_successor",
        "file": COVENANTS,
        "rule": "no other output carries the same successor script",
        "old": """        if (index != inputIndex &&
            tx.vout[index].scriptPubKey == expectedSuccessorScript) {
            return false;
        }""",
        "new": "        (void)index;",
    },
]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(cmd, cwd=REPO, timeout=1800):
    return subprocess.run(
        cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout
    )


def build(build_dir):
    return run(["cmake", "--build", build_dir, "--target", *TARGETS, "-j8"])


def run_tests(build_dir):
    """Return True when the covenant lane is fully green."""
    result = run(
        [
            "ctest",
            "--test-dir",
            build_dir,
            "-R",
            "^(%s)$" % "|".join(CTEST_TESTS),
            "--output-on-failure",
        ]
    )
    return result.returncode == 0, result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--only", help="run a single mutation by id")
    parser.add_argument("--json", help="write the report to this path")
    args = parser.parse_args()

    selected = MUTATIONS
    if args.only:
        selected = [m for m in MUTATIONS if m["id"] == args.only]
        if not selected:
            print("no such mutation: %s" % args.only, file=sys.stderr)
            return 2

    # Baseline must be green, or every "caught" verdict below is meaningless.
    print("=== baseline ===", flush=True)
    built = build(args.build_dir)
    if built.returncode != 0:
        print("baseline build FAILED:\n%s" % built.stderr[-4000:], file=sys.stderr)
        return 2
    green, output = run_tests(args.build_dir)
    if not green:
        print("baseline covenant lane is RED; fix that first:\n%s" % output[-4000:],
              file=sys.stderr)
        return 2
    print("baseline green: %d tests\n" % len(CTEST_TESTS), flush=True)

    results = []
    for mutation in selected:
        target = REPO / mutation["file"]
        original_bytes = target.read_bytes()
        original_digest = hashlib.sha256(original_bytes).hexdigest()
        original_text = original_bytes.decode()

        occurrences = original_text.count(mutation["old"])
        if occurrences != 1:
            print(
                "SKIP %-32s anchor matched %d times (expected exactly 1)"
                % (mutation["id"], occurrences)
            )
            results.append(
                {"id": mutation["id"], "verdict": "ANCHOR_STALE",
                 "occurrences": occurrences, "rule": mutation["rule"]}
            )
            continue

        started = time.time()
        try:
            target.write_text(
                original_text.replace(mutation["old"], mutation["new"], 1)
            )
            mutated_build = build(args.build_dir)
            if mutated_build.returncode != 0:
                # A mutation that does not compile is not evidence either way.
                verdict = "BUILD_FAILED"
            else:
                still_green, _ = run_tests(args.build_dir)
                verdict = "SURVIVED" if still_green else "caught"
        finally:
            # Restoration is the primary correctness property of this harness.
            target.write_bytes(original_bytes)
            restore_ok = sha256(target) == original_digest

        if not restore_ok:
            print(
                "\nFATAL: failed to restore %s (expected sha256 %s).\n"
                "Restore it from git before doing anything else."
                % (mutation["file"], original_digest),
                file=sys.stderr,
            )
            return 3

        elapsed = time.time() - started
        marker = {"caught": "ok  ", "SURVIVED": "GAP ",
                  "BUILD_FAILED": "n/a "}[verdict]
        print("%s %-32s %-8s %5.1fs  %s"
              % (marker, mutation["id"], verdict, elapsed, mutation["rule"]),
              flush=True)
        results.append(
            {"id": mutation["id"], "verdict": verdict,
             "rule": mutation["rule"], "seconds": round(elapsed, 1)}
        )

    caught = [r for r in results if r["verdict"] == "caught"]
    survived = [r for r in results if r["verdict"] == "SURVIVED"]
    stale = [r for r in results if r["verdict"] == "ANCHOR_STALE"]
    unbuildable = [r for r in results if r["verdict"] == "BUILD_FAILED"]

    scored = len(caught) + len(survived)
    print("\n=== mutation score ===")
    print("caught       %d" % len(caught))
    print("SURVIVED     %d" % len(survived))
    print("anchor stale %d" % len(stale))
    print("unbuildable  %d" % len(unbuildable))
    if scored:
        print("score        %d/%d (%.0f%%)"
              % (len(caught), scored, 100.0 * len(caught) / scored))

    for entry in survived:
        print("\nGAP: %s\n  rule: %s\n  Production was broken and the covenant "
              "lane stayed green. Add a test that fails without this rule."
              % (entry["id"], entry["rule"]))
    for entry in stale:
        print("\nSTALE ANCHOR: %s\n  The harness could not locate this rule "
              "uniquely. Production changed; update the mutation." % entry["id"])

    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(results, indent=2))

    # Stale anchors fail too: a mutation that silently stops applying would
    # otherwise quietly shrink coverage while still reporting 100%.
    return 0 if not survived and not stale else 1


if __name__ == "__main__":
    sys.exit(main())
