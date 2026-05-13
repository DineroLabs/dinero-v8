#!/usr/bin/env python3
"""
Compare multiple dinerod seeds at the same heights.

This is a release-gate tool for confirmed-chain consistency. It checks that
multiple seeds agree on:
  - tip height and tip hash
  - daemon identity and protocol/schema surface
  - block hash at sampled heights
  - raw header bytes and header-committed Utreexo root at sampled heights
  - compact filter identity at sampled heights
  - optionally, wallet.getproofbundle at the shared tip
  - optionally, wallet discovery parity via wallet.listaddresses and wallet.listunspent

Usage:
  python3 tools/check_seed_consistency.py \
    --seed a=http://127.0.0.1:20996,cookie=/tmp/node1/.cookie \
    --seed b=http://127.0.0.1:20997,cookie=/tmp/node2/.cookie \
    --samples 8 \
    --out /tmp/seed-consistency.json
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple


HEADER_HEX_LENGTH = 128 * 2
UTREEXO_ROOT_OFFSET = 68
UTREEXO_ROOT_END = 100
FILTER_COMMITMENT_PREFIX = bytes.fromhex("6a25444e524601")
DEFAULT_FILTER_ACTIVATION_HEIGHT = 11000
DEFAULT_RPC_RETRIES = 2
DEFAULT_RPC_RETRY_BACKOFF = 0.5
RETRYABLE_HTTP_STATUS = {429, 500, 502, 503, 504}
RETRYABLE_ERROR_SNIPPETS = (
    "Connection refused",
    "Connection reset by peer",
    "remote end closed connection",
    "Temporary failure in name resolution",
    "timed out",
)

RPC_RETRIES = DEFAULT_RPC_RETRIES
RPC_RETRY_BACKOFF = DEFAULT_RPC_RETRY_BACKOFF


@dataclass
class SeedConfig:
    name: str
    url: str
    cookie_path: Optional[Path] = None
    auth: Optional[str] = None

    def authorization_header(self) -> Optional[str]:
        if self.auth:
            token = base64.b64encode(self.auth.encode("utf-8")).decode("ascii")
            return f"Basic {token}"

        if self.cookie_path:
            cookie = self.cookie_path.read_text(encoding="utf-8").strip()
            if cookie:
                token = base64.b64encode(cookie.encode("utf-8")).decode("ascii")
                return f"Basic {token}"

        return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare multiple dinerod seeds")
    parser.add_argument(
        "--seed",
        action="append",
        required=True,
        help="Seed spec: name=url[,cookie=/path/to/.cookie][,auth=user:pass]",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=8,
        help="Number of heights to sample between genesis and the common tip",
    )
    parser.add_argument(
        "--height",
        action="append",
        type=int,
        default=[],
        help="Explicit height to include in the sample set (repeatable)",
    )
    parser.add_argument(
        "--activation-height",
        type=int,
        help="Include activation_height-1, activation_height, activation_height+1 in the sample set",
    )
    parser.add_argument(
        "--compare-proofbundle",
        action="store_true",
        help="Also compare wallet.getproofbundle at the common tip (only meaningful when every seed has the same wallet loaded)",
    )
    parser.add_argument(
        "--require-proofbundle",
        action="store_true",
        help="Fail if wallet.getproofbundle cannot be compared on every seed",
    )
    parser.add_argument(
        "--compare-wallet-discovery",
        action="store_true",
        help="Also compare wallet.listaddresses and wallet.listunspent (only meaningful when every seed has the same wallet loaded)",
    )
    parser.add_argument(
        "--require-wallet-discovery",
        action="store_true",
        help="Fail if wallet discovery parity cannot be compared on every seed",
    )
    parser.add_argument(
        "--proofbundle-max-utxos",
        type=int,
        default=16,
        help="max_utxos value passed to wallet.getproofbundle",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=15.0,
        help="RPC timeout in seconds",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=DEFAULT_RPC_RETRIES,
        help="Number of retries for transient RPC transport failures",
    )
    parser.add_argument(
        "--retry-backoff",
        type=float,
        default=DEFAULT_RPC_RETRY_BACKOFF,
        help="Seconds to sleep between transient RPC retries",
    )
    parser.add_argument(
        "--out",
        help="Optional JSON report path",
    )
    return parser.parse_args()


def parse_seed_spec(spec: str) -> SeedConfig:
    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts or "=" not in parts[0]:
        raise ValueError(f"Invalid seed spec: {spec}")

    name, url = parts[0].split("=", 1)
    if not name or not url:
        raise ValueError(f"Invalid seed spec: {spec}")

    cookie_path: Optional[Path] = None
    auth: Optional[str] = None

    for part in parts[1:]:
        if "=" not in part:
            raise ValueError(f"Invalid seed option in {spec}: {part}")
        key, value = part.split("=", 1)
        key = key.strip().lower()
        value = value.strip()
        if key == "cookie":
            cookie_path = Path(value)
        elif key == "auth":
            auth = value
        else:
            raise ValueError(f"Unknown seed option in {spec}: {key}")

    return SeedConfig(name=name, url=url, cookie_path=cookie_path, auth=auth)


def is_retryable_transport_error(exc: Exception) -> bool:
    if isinstance(exc, urllib.error.HTTPError):
        return exc.code in RETRYABLE_HTTP_STATUS
    message = str(exc)
    return any(snippet in message for snippet in RETRYABLE_ERROR_SNIPPETS)


def json_rpc(seed: SeedConfig, method: str, params: Sequence[Any], timeout: float) -> Any:
    payload = json.dumps(
        {"jsonrpc": "2.0", "id": f"{seed.name}:{method}", "method": method, "params": list(params)}
    ).encode("utf-8")
    auth_header = seed.authorization_header()
    last_error: Optional[Exception] = None

    for attempt in range(RPC_RETRIES + 1):
        request = urllib.request.Request(
            seed.url,
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        if auth_header:
            request.add_header("Authorization", auth_header)

        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                raw = response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            if attempt < RPC_RETRIES and is_retryable_transport_error(exc):
                last_error = RuntimeError(f"HTTP {exc.code}: {detail}")
                time.sleep(RPC_RETRY_BACKOFF)
                continue
            raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc
        except OSError as exc:
            if attempt < RPC_RETRIES and is_retryable_transport_error(exc):
                last_error = exc
                time.sleep(RPC_RETRY_BACKOFF)
                continue
            raise RuntimeError(str(exc)) from exc

        try:
            decoded = json.loads(raw)
        except json.JSONDecodeError as exc:
            if attempt < RPC_RETRIES:
                last_error = RuntimeError(f"Invalid JSON response: {exc}")
                time.sleep(RPC_RETRY_BACKOFF)
                continue
            raise RuntimeError(f"Invalid JSON response: {exc}") from exc

        rpc_error = decoded.get("error")
        if rpc_error:
            if isinstance(rpc_error, dict):
                message = rpc_error.get("message", rpc_error)
            else:
                message = rpc_error
            raise RuntimeError(str(message))

        result = decoded.get("result")
        if isinstance(result, dict) and result.get("error"):
            nested = result["error"]
            if isinstance(nested, dict):
                nested = nested.get("message", nested)
            raise RuntimeError(str(nested))

        return result

    if last_error is not None:
        raise RuntimeError(str(last_error))
    raise RuntimeError("RPC request failed without a response")


def is_method_not_found_error(exc: Exception) -> bool:
    return "Method not found" in str(exc)


def try_json_rpc_methods(
    seed: SeedConfig,
    methods: Sequence[str],
    params: Sequence[Any],
    timeout: float,
) -> Tuple[Optional[str], Dict[str, Any]]:
    last_error: Optional[Exception] = None
    for method in methods:
        try:
            result = json_rpc(seed, method, params, timeout)
            if isinstance(result, dict):
                return method, result
            return method, {}
        except Exception as exc:  # noqa: BLE001
            last_error = exc
            if is_method_not_found_error(exc):
                continue
            raise

    if last_error is not None and not is_method_not_found_error(last_error):
        raise last_error
    return None, {}


def normalize_json(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: normalize_json(value[key]) for key in sorted(value)}
    if isinstance(value, list):
        return [normalize_json(item) for item in value]
    return value


def read_compact_size(raw: bytes, offset: int) -> Tuple[int, int]:
    if offset >= len(raw):
        raise ValueError("unexpected end of transaction")

    first = raw[offset]
    offset += 1

    if first < 0xFD:
        return first, offset
    if first == 0xFD:
        if offset + 2 > len(raw):
            raise ValueError("truncated compact size")
        return int.from_bytes(raw[offset : offset + 2], "little"), offset + 2
    if first == 0xFE:
        if offset + 4 > len(raw):
            raise ValueError("truncated compact size")
        return int.from_bytes(raw[offset : offset + 4], "little"), offset + 4
    if offset + 8 > len(raw):
        raise ValueError("truncated compact size")
    return int.from_bytes(raw[offset : offset + 8], "little"), offset + 8


def decode_output_scripts(tx_hex: str) -> List[bytes]:
    raw = bytes.fromhex(tx_hex)
    offset = 0

    if len(raw) < 10:
        raise ValueError("transaction too short")

    offset += 4  # version

    has_witness = False
    if offset + 2 <= len(raw) and raw[offset] == 0 and raw[offset + 1] == 1:
        has_witness = True
        offset += 2

    vin_count, offset = read_compact_size(raw, offset)
    for _ in range(vin_count):
        offset += 32 + 4
        script_len, offset = read_compact_size(raw, offset)
        offset += script_len + 4
        if offset > len(raw):
            raise ValueError("truncated input")

    vout_count, offset = read_compact_size(raw, offset)
    scripts: List[bytes] = []
    for _ in range(vout_count):
        offset += 8
        script_len, offset = read_compact_size(raw, offset)
        script = raw[offset : offset + script_len]
        if len(script) != script_len:
            raise ValueError("truncated output script")
        scripts.append(script)
        offset += script_len

    if has_witness:
        for _ in range(vin_count):
            stack_count, offset = read_compact_size(raw, offset)
            for _ in range(stack_count):
                item_len, offset = read_compact_size(raw, offset)
                offset += item_len
                if offset > len(raw):
                    raise ValueError("truncated witness item")

    if offset + 4 > len(raw):
        raise ValueError("missing locktime")

    return scripts


def extract_filter_commitment(coinbase_tx_hex: str) -> Dict[str, Any]:
    try:
        scripts = decode_output_scripts(coinbase_tx_hex)
    except ValueError as exc:
        return {"present": False, "parse_error": str(exc)}

    for index in range(len(scripts) - 1, -1, -1):
        script = scripts[index]
        if len(script) >= len(FILTER_COMMITMENT_PREFIX) + 32 and script.startswith(FILTER_COMMITMENT_PREFIX):
            raw_hash = script[len(FILTER_COMMITMENT_PREFIX) : len(FILTER_COMMITMENT_PREFIX) + 32]
            return {
                "present": True,
                "index": index,
                "script_hex": script.hex(),
                "committed_filter_hash": raw_hash[::-1].hex(),
            }

    return {"present": False}


def extract_utreexo_root(header_hex: str) -> str:
    if len(header_hex) != HEADER_HEX_LENGTH:
        raise ValueError(f"Unexpected header size: {len(header_hex) // 2} bytes")
    raw = bytes.fromhex(header_hex)
    return raw[UTREEXO_ROOT_OFFSET:UTREEXO_ROOT_END].hex()


def fetch_tip(seed: SeedConfig, timeout: float) -> Dict[str, Any]:
    height = int(json_rpc(seed, "getblockcount", [], timeout))
    block_hash = str(json_rpc(seed, "getblockhash", [height], timeout))
    return {"height": height, "block_hash": block_hash}


def fetch_seed_identity(seed: SeedConfig, timeout: float) -> Dict[str, Any]:
    rpc_version = json_rpc(seed, "rpc.version", [], timeout)
    blockchain_info = json_rpc(seed, "getblockchaininfo", [], timeout)
    network_method, network_info = try_json_rpc_methods(
        seed,
        ["getnetworkinfo", "network.getinfo", "server.getinfo"],
        [],
        timeout,
    )

    return {
        "version": rpc_version.get("version"),
        "git_sha": rpc_version.get("git_sha"),
        "repo": rpc_version.get("repo"),
        "component": rpc_version.get("component"),
        "schema": rpc_version.get("schema"),
        "protocol_version": rpc_version.get("protocol_version"),
        "chain": blockchain_info.get("chain"),
        "network_info_method": network_method,
        "subversion": network_info.get("subversion"),
        "network_protocol_version": network_info.get("protocolversion"),
        "localservices": network_info.get("localservices"),
    }


def fetch_header(seed: SeedConfig, height: int, timeout: float) -> Dict[str, Any]:
    response = json_rpc(seed, "blockchain.getheaders", [height, 1], timeout)
    headers_hex = str(response.get("headers", ""))
    count = int(response.get("count", 0))
    if count != 1 or not headers_hex:
        return {"present": False, "count": count}
    return {
        "present": True,
        "count": count,
        "header_hex": headers_hex,
        "utreexo_root": extract_utreexo_root(headers_hex),
    }


def fetch_filter(seed: SeedConfig, height: int, timeout: float) -> Dict[str, Any]:
    response = json_rpc(seed, "blockchain.getblockfilters", [height, 1], timeout)
    filters = response.get("filters", [])
    if not filters:
        return {"present": False, "count": int(response.get("count", 0))}

    entry = filters[0]
    coinbase_tx = entry.get("coinbase_tx")
    commitment = extract_filter_commitment(str(coinbase_tx)) if coinbase_tx else {"present": False}
    return {
        "present": True,
        "count": int(response.get("count", 0)),
        "block_hash": entry.get("block_hash"),
        "filter_hex": entry.get("filter"),
        "filter_hash": entry.get("filter_hash"),
        "element_count": entry.get("element_count"),
        "coinbase_tx": coinbase_tx,
        "coinbase_txid": entry.get("coinbase_txid"),
        "merkle_proof": entry.get("merkle_proof", []),
        "filter_commitment_present": commitment.get("present", False),
        "filter_commitment_index": commitment.get("index"),
        "filter_commitment_hash": commitment.get("committed_filter_hash"),
        "filter_commitment_parse_error": commitment.get("parse_error"),
        "tx_count": entry.get("tx_count"),
    }


def summarize_coinbase_filter_record(filter_record: Dict[str, Any]) -> Dict[str, Any]:
    if not filter_record.get("present"):
        return {
            "present": False,
            "count": filter_record.get("count"),
        }

    return {
        "present": True,
        "block_hash": filter_record.get("block_hash"),
        "coinbase_tx": filter_record.get("coinbase_tx"),
        "coinbase_txid": filter_record.get("coinbase_txid"),
        "filter_hash": filter_record.get("filter_hash"),
        "filter_commitment_present": filter_record.get("filter_commitment_present"),
        "filter_commitment_index": filter_record.get("filter_commitment_index"),
        "filter_commitment_hash": filter_record.get("filter_commitment_hash"),
        "filter_commitment_parse_error": filter_record.get("filter_commitment_parse_error"),
    }


def fetch_proofbundle(seed: SeedConfig, timeout: float, max_utxos: int) -> Dict[str, Any]:
    response = json_rpc(
        seed,
        "wallet.getproofbundle",
        [{"min_confirmations": 1, "spendable_only": False, "max_utxos": max_utxos}],
        timeout,
    )

    proofs = []
    for proof in response.get("proofs", []):
        proofs.append(
            {
                "txid": proof.get("txid"),
                "vout": proof.get("vout"),
                "success": proof.get("success"),
                "amount_una": proof.get("amount_una"),
                "leaf_hash": proof.get("leaf_hash"),
                "position": proof.get("position"),
                "num_leaves": proof.get("num_leaves"),
                "siblings": proof.get("siblings", []),
            }
        )
    proofs.sort(key=lambda item: (str(item["txid"]), int(item["vout"] or 0)))

    return {
        "accumulator_root": response.get("accumulator_root"),
        "block_hash": response.get("block_hash"),
        "height": response.get("height"),
        "utxo_count": response.get("utxo_count"),
        "truncated": response.get("truncated"),
        "proofs": proofs,
    }


def summarize_wallet_addresses(rows: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
    summarized: List[Dict[str, Any]] = []
    for row in rows:
        address = row.get("address")
        if not address:
            continue
        summarized.append(
            {
                "account": row.get("account"),
                "address": address,
                "balance": row.get("balance"),
                "confirmed": row.get("confirmed"),
                "spendable": row.get("spendable"),
                "utxo_count": row.get("utxo_count"),
                "index": row.get("index"),
                "change": row.get("change"),
                "external": row.get("external"),
                "path": row.get("path"),
                "type": row.get("type"),
            }
        )
    summarized.sort(key=lambda item: str(item["address"]))
    return summarized


def summarize_wallet_unspent(rows: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
    summarized: List[Dict[str, Any]] = []
    for row in rows:
        txid = row.get("txid")
        vout = row.get("vout")
        if txid is None or vout is None:
            continue
        summarized.append(
            {
                "txid": txid,
                "vout": vout,
                "address": row.get("address"),
                "amount_una": row.get("amount_una"),
                "confirmations": row.get("confirmations"),
                "is_coinbase": row.get("is_coinbase"),
                "is_mature": row.get("is_mature"),
                "locked": row.get("locked"),
                "safe": row.get("safe"),
                "spendable": row.get("spendable"),
                "derivation_path": row.get("derivation_path"),
            }
        )
    summarized.sort(key=lambda item: (str(item["txid"]), int(item["vout"])))
    return summarized


def fetch_wallet_discovery(seed: SeedConfig, timeout: float) -> Dict[str, Any]:
    addresses = json_rpc(seed, "wallet.listaddresses", [], timeout)
    unspent = json_rpc(seed, "wallet.listunspent", [], timeout)

    addresses_rows = addresses if isinstance(addresses, list) else []
    unspent_rows = unspent if isinstance(unspent, list) else []

    return {
        "address_count": len(addresses_rows),
        "funded_address_count": sum(1 for row in addresses_rows if int(row.get("utxo_count", 0)) > 0),
        "addresses": summarize_wallet_addresses(addresses_rows),
        "unspent_count": len(unspent_rows),
        "unspent": summarize_wallet_unspent(unspent_rows),
    }


def summarize_mismatch(records: Dict[str, Any]) -> Dict[str, Any]:
    return {name: normalize_json(record) for name, record in records.items()}


def compare_records(label: str, records: Dict[str, Any]) -> Tuple[bool, Optional[Dict[str, Any]]]:
    normalized = {name: normalize_json(record) for name, record in records.items()}
    encoded = {name: json.dumps(record, sort_keys=True) for name, record in normalized.items()}
    unique = set(encoded.values())
    if len(unique) == 1:
        return True, None
    return False, {"label": label, "records": normalized}


def compute_sample_heights(common_tip: int, args: argparse.Namespace) -> List[int]:
    heights = set(max(0, height) for height in args.height)

    if args.activation_height is not None:
        for delta in (-1, 0, 1):
            heights.add(max(0, args.activation_height + delta))

    sample_count = max(1, args.samples)
    if common_tip == 0:
        heights.add(0)
    elif sample_count == 1:
        heights.add(common_tip)
    else:
        for index in range(sample_count):
            height = round((common_tip * index) / (sample_count - 1))
            heights.add(int(height))

    return sorted(height for height in heights if height <= common_tip)


def write_report(path: str, report: Dict[str, Any]) -> None:
    output_path = Path(path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def validate_filter_record(
    record: Dict[str, Any],
    height: int,
    activation_height: int,
) -> List[str]:
    errors: List[str] = []

    if not record.get("present"):
        return errors

    if record.get("coinbase_tx") and record.get("filter_commitment_parse_error"):
        errors.append(f"coinbase_tx_parse_error={record['filter_commitment_parse_error']}")

    commitment_present = bool(record.get("filter_commitment_present"))
    committed_hash = record.get("filter_commitment_hash")
    filter_hash = record.get("filter_hash")

    if commitment_present and committed_hash != filter_hash:
        errors.append("filter_commitment_mismatch")

    if height >= activation_height and not commitment_present:
        errors.append("missing_filter_commitment_post_activation")

    return errors


def main() -> int:
    args = parse_args()
    global RPC_RETRIES, RPC_RETRY_BACKOFF
    RPC_RETRIES = max(0, args.retries)
    RPC_RETRY_BACKOFF = max(0.0, args.retry_backoff)

    try:
        seeds = [parse_seed_spec(spec) for spec in args.seed]
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if len(seeds) < 2:
        print("error: at least two --seed values are required", file=sys.stderr)
        return 2

    report: Dict[str, Any] = {
        "ok": True,
        "seeds": {},
        "mismatches": [],
        "proofbundle": None,
        "wallet_discovery": None,
        "sampled_heights": [],
    }
    effective_activation_height = (
        args.activation_height if args.activation_height is not None else DEFAULT_FILTER_ACTIVATION_HEIGHT
    )
    report["filter_activation_height"] = effective_activation_height

    tip_info: Dict[str, Dict[str, Any]] = {}
    seed_identities: Dict[str, Dict[str, Any]] = {}
    for seed in seeds:
        try:
            tip = fetch_tip(seed, args.timeout)
            identity = fetch_seed_identity(seed, args.timeout)
            tip_info[seed.name] = tip
            seed_identities[seed.name] = identity
            report["seeds"][seed.name] = {"url": seed.url, "tip": tip, "identity": identity}
        except Exception as exc:  # noqa: BLE001
            report["ok"] = False
            report["mismatches"].append(
                {"label": "seed_metadata_fetch_error", "records": {seed.name: {"error": str(exc)}}}
            )

    if len(tip_info) != len(seeds):
        if args.out:
            write_report(args.out, report)
        print("FAIL: could not fetch metadata from every seed")
        return 1

    tips_ok, tip_mismatch = compare_records("tip", tip_info)
    if not tips_ok:
        report["ok"] = False
        report["mismatches"].append(tip_mismatch)

    identities_ok, identity_mismatch = compare_records("seed_identity", seed_identities)
    if not identities_ok:
        report["ok"] = False
        report["mismatches"].append(identity_mismatch)

    common_tip = min(tip["height"] for tip in tip_info.values())
    sampled_heights = compute_sample_heights(common_tip, args)
    report["common_tip_height"] = common_tip
    report["sampled_heights"] = sampled_heights
    report["height_checks"] = []

    for height in sampled_heights:
        height_report: Dict[str, Any] = {"height": height, "ok": True, "mismatches": []}
        block_hashes: Dict[str, Any] = {}
        headers: Dict[str, Any] = {}
        filters: Dict[str, Any] = {}
        coinbase_filters: Dict[str, Any] = {}

        for seed in seeds:
            try:
                block_hashes[seed.name] = json_rpc(seed, "getblockhash", [height], args.timeout)
                headers[seed.name] = fetch_header(seed, height, args.timeout)
                filters[seed.name] = fetch_filter(seed, height, args.timeout)
                coinbase_filters[seed.name] = summarize_coinbase_filter_record(filters[seed.name])
            except Exception as exc:  # noqa: BLE001
                height_report["ok"] = False
                height_report["mismatches"].append(
                    {
                        "label": "fetch_error",
                        "records": {seed.name: {"error": str(exc)}},
                    }
                )

        for seed in seeds:
            if seed.name not in filters:
                continue
            filter_errors = validate_filter_record(filters[seed.name], height, effective_activation_height)
            if filter_errors:
                report["ok"] = False
                height_report["ok"] = False
                height_report["mismatches"].append(
                    {
                        "label": "filter_invariant_error",
                        "records": {
                            seed.name: {
                                "errors": filter_errors,
                                "filter": normalize_json(filters[seed.name]),
                            }
                        },
                    }
                )

        for label, records in (
            ("block_hash", block_hashes),
            ("header", headers),
            ("coinbase_filter_commitment", coinbase_filters),
            ("filter", filters),
        ):
            if len(records) != len(seeds):
                report["ok"] = False
                height_report["ok"] = False
                continue

            same, mismatch = compare_records(label, records)
            if not same:
                report["ok"] = False
                height_report["ok"] = False
                height_report["mismatches"].append(mismatch)

        report["height_checks"].append(height_report)

    if args.compare_proofbundle:
        proofbundle_records: Dict[str, Any] = {}
        proofbundle_errors: Dict[str, Any] = {}

        for seed in seeds:
            try:
                proofbundle_records[seed.name] = fetch_proofbundle(
                    seed, args.timeout, args.proofbundle_max_utxos
                )
            except Exception as exc:  # noqa: BLE001
                proofbundle_errors[seed.name] = {"error": str(exc)}

        if proofbundle_errors:
            report["proofbundle"] = {"ok": False, "errors": proofbundle_errors}
            if args.require_proofbundle:
                report["ok"] = False
                report["mismatches"].append(
                    {"label": "proofbundle_fetch_error", "records": proofbundle_errors}
                )
        else:
            same, mismatch = compare_records("proofbundle", proofbundle_records)
            report["proofbundle"] = {"ok": same, "records": summarize_mismatch(proofbundle_records)}
            if not same:
                report["ok"] = False
                report["mismatches"].append(mismatch)

    if args.compare_wallet_discovery:
        wallet_records: Dict[str, Any] = {}
        wallet_errors: Dict[str, Any] = {}

        for seed in seeds:
            try:
                wallet_records[seed.name] = fetch_wallet_discovery(seed, args.timeout)
            except Exception as exc:  # noqa: BLE001
                wallet_errors[seed.name] = {"error": str(exc)}

        if wallet_errors:
            report["wallet_discovery"] = {"ok": False, "errors": wallet_errors}
            if args.require_wallet_discovery:
                report["ok"] = False
                report["mismatches"].append(
                    {"label": "wallet_discovery_fetch_error", "records": wallet_errors}
                )
        else:
            same, mismatch = compare_records("wallet_discovery", wallet_records)
            report["wallet_discovery"] = {"ok": same, "records": summarize_mismatch(wallet_records)}
            if not same:
                report["ok"] = False
                report["mismatches"].append(mismatch)

    if args.out:
        write_report(args.out, report)

    if report["ok"]:
        print(
            f"PASS: {len(seeds)} seeds consistent at {len(sampled_heights)} sampled heights"
            + (" with proofbundle comparison" if args.compare_proofbundle else "")
            + (" and wallet discovery comparison" if args.compare_wallet_discovery else "")
        )
        print(f"Common tip height: {common_tip}")
        return 0

    print("FAIL: seed inconsistency detected")
    for mismatch in report["mismatches"]:
        print(f"- {mismatch['label']}")
    if args.out:
        print(f"Report: {args.out}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
