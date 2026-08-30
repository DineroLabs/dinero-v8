#!/usr/bin/env python3
"""Release tripwire for mutating RPC canonical names and flat aliases."""
from pathlib import Path
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/daemon/http_rpc_server.cpp").read_text()
match = re.search(r"ADMIN_METHODS\s*=\s*\{(.*?)\};", SOURCE, re.S)
if not match:
    raise SystemExit("ADMIN_METHODS not found")
admin = set(re.findall(r'"([a-zA-Z0-9_.]+)"', match.group(1)))

required = {
    "wallet.sendtoaddress", "wallet.sendmany", "wallet.consolidate",
    "wallet.shield", "wallet.unshield", "wallet.transfer",
    "wallet.signrawtransaction", "wallet.signpsbt", "sendrawtransaction",
    "vault.observe", "vault.withdraw", "vault.processnext", "vault.setoperator",
    "contract.createescrow", "contract.setlocktx", "contract.release",
    "contract.refund", "contract.broadcastrelease", "contract.broadcastrefund",
    "blockchain.invalidateblock", "blockchain.reconsiderblock",
}
missing = sorted(required - admin)
if missing:
    raise SystemExit("missing admin RPC classifications: " + ", ".join(missing))

flat = {name.split(".", 1)[1] for name in admin if "." in name}
required_aliases = {name.split(".", 1)[1] for name in required if "." in name}
missing_aliases = sorted(required_aliases - flat)
if missing_aliases:
    raise SystemExit("flat aliases lack inherited admin classification: " + ", ".join(missing_aliases))

manifest = {
    "schema": "dinero.rpc.release-policy.v1",
    "admin_canonical": sorted(admin),
    "admin_flat_aliases": sorted(flat),
    "contract_fund_movement": "disabled_pending_bound_signing_package",
    "shared_rpc_mobile_fund_movement": "disabled_for_vault_contract_shielded",
}
output = ROOT / "build" / "readiness" / "rpc-release-policy.json"
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(manifest, indent=2) + "\n")
print(f"PASS: {len(admin)} canonical admin methods and {len(flat)} flat aliases; wrote {output}")
