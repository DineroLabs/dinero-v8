#!/usr/bin/env python3
"""Exercise the parity gate's real submit evaluator with mocked RPC replies."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def shell_function(source: str, name: str) -> str:
    match = re.search(rf"(?ms)^{re.escape(name)}\(\) \{{\n.*?^\}}", source)
    if match is None:
        raise AssertionError(f"missing shell function: {name}")
    return match.group()


def evaluate(script: Path, response: str, before: str = "129", after: str = "129",
             before_exit: int = 0, after_exit: int = 0) -> tuple[str, str, str, str, str]:
    source = script.read_text()
    functions = "\n".join(shell_function(source, name) for name in
                          ("classify_fixture_outcome", "submit_with_height_eval"))
    mocks = r'''
rpc_scalar() {
    local count=0
    [[ -f "$MOCK_COUNT_FILE" ]] && count=$(cat "$MOCK_COUNT_FILE")
    count=$((count + 1))
    printf '%s\n' "$count" > "$MOCK_COUNT_FILE"
    if [[ "$count" -eq 1 ]]; then
        [[ "$MOCK_BEFORE_EXIT" -eq 0 ]] || return "$MOCK_BEFORE_EXIT"
        printf '%s\n' "$MOCK_BEFORE"
        return 0
    fi
    [[ "$MOCK_AFTER_EXIT" -eq 0 ]] || return "$MOCK_AFTER_EXIT"
    printf '%s\n' "$MOCK_AFTER"
    return 0
}
rpc_raw() {
    printf '%s' "$MOCK_RESPONSE"
    return 0
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        env = os.environ | {
            "MOCK_COUNT_FILE": str(Path(tmp) / "rpc-count"),
            "MOCK_RESPONSE": response,
            "MOCK_BEFORE": before,
            "MOCK_AFTER": after,
            "MOCK_BEFORE_EXIT": str(before_exit),
            "MOCK_AFTER_EXIT": str(after_exit),
            "WORKDIR": tmp,
        }
        invocation = r'''
reply=$(submit_with_height_eval ignored 12345 deadbeef)
IFS='|' read -r status height_before height_after reason <<< "$reply"
printf '%s\n' "$status" "$height_before" "$height_after" "$reason"
classify_fixture_outcome "$status" "$height_before" "$height_after" "$reason"
'''
        result = subprocess.run(
            ["bash", "-s"], input="set -euo pipefail\n" + mocks + functions + invocation,
            text=True, capture_output=True, env=env,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        if len(lines) != 5:
            raise AssertionError(f"unexpected evaluator output: {result.stdout!r}")
        status, height_before, height_after, reason, outcome = lines
        if "evidence=" in reason:
            evidence_path = Path(reason.split("evidence=", 1)[1])
            if evidence_path.read_text() != response:
                raise AssertionError("malformed RPC evidence did not preserve raw reply")
        return status, height_before, height_after, reason, outcome


def main() -> None:
    script = Path(sys.argv[1]).resolve()
    consensus_error = '{"error":{"message":"bad-witness-commitment: mismatch"},"result":null}'
    assert evaluate(script, consensus_error)[-1] == "REJECT"

    failed_after = evaluate(script, consensus_error, after_exit=28)
    assert failed_after[-1] == "ERROR", f"post-submit RPC failure was normalized: {failed_after}"
    assert failed_after[2] == "RPC_DOWN", f"failed after-read lost its marker: {failed_after}"

    empty_after = evaluate(script, consensus_error, after="")
    assert empty_after[-1] == "ERROR", f"empty after-height was normalized: {empty_after}"

    empty_before = evaluate(script, consensus_error, before="")
    assert empty_before[-1] == "ERROR", f"empty before-height was accepted: {empty_before}"

    for malformed in ("", "{}", "not-json", '{"error":null}'):
        outcome = evaluate(script, malformed)
        assert outcome[-1] == "ERROR", f"malformed RPC reply was normalized: {outcome}"
        assert "evidence=" in outcome[3], f"malformed reply lacks raw evidence: {outcome}"

    assert evaluate(script, '{"result":null,"error":null}', after="130")[-1] == "ACCEPT"
    assert evaluate(script, '{"result":"bad-block","error":null}')[-1] == "REJECT"
    print("PASS: parity RPC failures cannot normalize to consensus rejection")


if __name__ == "__main__":
    main()
