#!/usr/bin/env python3
"""Exercise the wallet-cycle HTTP polling contract without a daemon or network."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location(
    "wallet_cycle", Path(__file__).with_name("test_orchard_daemon_wallet_cycle.py"))
cycle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cycle)


class HttpContract(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="orchard-http-unit-")
        self.addCleanup(temporary.cleanup)
        self.node = cycle.Node.__new__(cycle.Node)
        self.node.root = self.node.datadir = Path(temporary.name)
        (self.node.datadir / ".cookie").write_text("unit:fake")
        self.node.process = SimpleNamespace(poll=lambda: None)
        self.node.ports, self.node.starts = [1], 1
        self.request = {"account": 3, "request_id": "unit-request"}
        self.success = {"admitted": True, "durable_state": "signed", "txid": "unit-tx"}

    def reply(self, value):
        return io.StringIO(json.dumps(value))

    def arrange_ready(self, kind):
        self.node.rpc = Mock(side_effect=[
            {"operation_id": self.request["request_id"], "durable_state": "reserved"},
            self.success.copy()])
        self.node.status = Mock(return_value={
            "durable_state": "signed", "txid": "unit-tx", "chain_observation": None})
        self.node.restart = Mock()
        self.attempts = 0

        def wait(predicate, label, seconds):
            self.assertEqual(seconds, 180)
            for _ in range(3):
                self.attempts += 1
                if predicate():
                    return
            self.fail("unexpected continued polling")
        self.node.wait = wait

    def assert_ready_after(self, kind, first):
        self.arrange_ready(kind)
        with patch.object(cycle.urllib.request, "urlopen", side_effect=[
                self.reply(first), self.reply({"result": self.success})]) as transport:
            self.assertEqual(self.node.ready(kind, self.request), "unit-tx")
        self.assertEqual(self.attempts, 2)
        self.assertEqual(transport.call_count, 2)
        self.node.restart.assert_called_once_with()
        self.assertEqual(self.node.rpc.call_args_list[1].args,
                         ("wallet.orchard.finish" + kind, self.request))

    def assert_error_propagates(self, error, kind="shield"):
        self.arrange_ready(kind)
        with patch.object(cycle.urllib.request, "urlopen", return_value=self.reply({"error": error})) as transport:
            with self.assertRaises(cycle.RpcError):
                self.node.ready(kind, self.request)
        self.assertEqual(self.attempts, 1)
        transport.assert_called_once()
        self.node.restart.assert_not_called()

    def test_raw_preserves_error_identity(self):
        method = "wallet.orchard.finishshield"
        with patch.object(cycle.urllib.request, "urlopen", return_value=self.reply({
                "error": {"code": -32603, "message": "Owned shield proof is not complete"}})):
            with self.assertRaises(cycle.RpcError) as caught:
                self.node.raw(method, self.request)
        self.assertEqual(caught.exception.method, method)
        self.assertEqual(caught.exception.code, -32603)
        self.assertEqual(caught.exception.rpc_message, "Owned shield proof is not complete")

    def test_shield_pending_then_ready(self):
        self.assert_ready_after("shield", {"error": {
            "code": -32603, "message": "Owned shield proof is not complete"}})

    def test_spend_pending_then_ready(self):
        self.assert_ready_after("spend", {"error": {
            "code": -32603, "message": "Owned Orchard proof is not available; reservation retained"}})

    def test_wrong_code_propagates(self):
        for code in (-32000, "-32603", None, True, -32603.0):
            with self.subTest(code=code):
                self.assert_error_propagates({"code": code, "message": "Owned shield proof is not complete"})

    def test_unexpected_message_propagates(self):
        self.assert_error_propagates({"code": -32603, "message": "Proof failed"})

    def test_malformed_error_propagates(self):
        for error in ("Owned shield proof is not complete", {}, {"code": -32603}):
            with self.subTest(error=error):
                self.assert_error_propagates(error)

    def test_wrong_kind_message_propagates(self):
        self.assert_error_propagates({"code": -32603,
            "message": "Owned Orchard proof is not available; reservation retained"})
        self.assert_error_propagates({"code": -32603,
            "message": "Owned shield proof is not complete"}, kind="spend")

    def test_network_failure_propagates(self):
        self.arrange_ready("shield")
        with patch.object(cycle.urllib.request, "urlopen", side_effect=OSError("unit transport failure")):
            with self.assertRaisesRegex(OSError, "unit transport failure"):
                self.node.ready("shield", self.request)
        self.assertEqual(self.attempts, 1)
        self.node.restart.assert_not_called()

    def test_wrong_method_propagates(self):
        self.arrange_ready("shield")
        error = cycle.RpcError("wrong method", method="wallet.orchard.queueshield",
                               error={"code": -32603, "message": "Owned shield proof is not complete"})
        self.node.raw = Mock(side_effect=error)
        with self.assertRaises(cycle.RpcError) as caught:
            self.node.ready("shield", self.request)
        self.assertIs(caught.exception, error)
        self.node.restart.assert_not_called()

    def test_legacy_nested_pending_then_ready(self):
        self.assert_ready_after("shield", {"result": {"error": "Owned shield proof is not complete"}})


class ProfileContract(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="orchard-profile-unit-")
        self.addCleanup(temporary.cleanup)
        self.node = cycle.Node.__new__(cycle.Node)
        self.node.datadir = Path(temporary.name)
        self.node.coupled_release = True
        self.node.pow_profile = None
        self.checksum = "a" * 64
        (self.node.datadir / "regtest-pow-profile").write_text(
            "regtest-pow-profile-v1\n" + self.checksum + "\n")

    def arrange(self, height, spacing, compact):
        profile = {"regtest_pow_enforced": True, "target_spacing_height": height + 1,
                   "sixty_second_activation_height": 102, "target_spacing_seconds": spacing,
                   "shielded_compact_activation_height": 102, "shielded_compact_active": compact,
                   "consensus_checksum": self.checksum, "network": "regtest",
                   "genesis_hash": "b" * 64, "genesis_bits": 0x1d31ffce,
                   "genesis_time": 1776384000, "pow_limit_bits": "0x207fffff"}
        self.node.rpc = Mock(side_effect=[height, profile])
        return profile

    def test_transition_rewind_and_reactivation_use_next_height(self):
        for height, spacing, compact in ((0, 120, False), (100, 120, False),
                (101, 60, True), (102, 60, True), (100, 120, False), (102, 60, True)):
            with self.subTest(height=height, spacing=spacing):
                self.arrange(height, spacing, compact)
                self.assertEqual(self.node.check_profile(), (height, spacing))
                self.assertEqual(self.node.pow_profile, self.checksum)

    def test_parent_must_report_next_block_rules(self):
        self.arrange(101, 120, False)
        with self.assertRaises(AssertionError):
            self.node.check_profile()

    def test_partial_schedule_or_wrong_height_refuses(self):
        for key, value in (("target_spacing_height", 101),
                           ("shielded_compact_activation_height", 103),
                           ("sixty_second_activation_height", 103),
                           ("shielded_compact_active", 1),
                           ("target_spacing_seconds", 60.0),
                           ("regtest_pow_enforced", False)):
            with self.subTest(key=key):
                self.arrange(101, 60, True)[key] = value
                with self.assertRaises(AssertionError):
                    self.node.check_profile()

    def test_profile_change_across_restart_refuses(self):
        self.arrange(100, 120, False)
        self.node.check_profile()
        changed = "b" * 64
        self.arrange(102, 60, True)["consensus_checksum"] = changed
        (self.node.datadir / "regtest-pow-profile").write_text(
            "regtest-pow-profile-v1\n" + changed + "\n")
        with self.assertRaises(AssertionError):
            self.node.check_profile()

    def test_existing_height_one_profile_keeps_sixty_second_expectation(self):
        self.node.coupled_release = False
        self.arrange(0, 60, False)["sixty_second_activation_height"] = 1
        self.assertEqual(self.node.check_profile(), (0, 60))


    def test_regtest_genesis_and_anchor_remain_distinct(self):
        self.arrange(0, 120, False)
        self.node.check_profile()
        self.assertEqual(self.node.historical_pow_bits, 0x207fffff)
        self.assertEqual(self.node.historical_genesis,
            {"hash": "b" * 64, "bits": 0x1d31ffce, "time": 1776384000})
        for key, value in (("network", "mainnet"), ("pow_limit_bits", "0x1d31ffce"),
                           ("genesis_bits", 0x207fffff), ("genesis_bits", True),
                           ("genesis_time", 1776384001), ("genesis_hash", "not-a-hash")):
            with self.subTest(key=key, value=value):
                self.arrange(0, 120, False)[key] = value
                with self.assertRaises(AssertionError):
                    self.node.check_profile()


class HistoricalTemplateContract(unittest.TestCase):
    def setUp(self):
        self.genesis={'time':1776384000,'bits':0x1d31ffce,'hash':'aa'*32,'height':0}
        self.anchor_bits=0x207fffff
        self.time_anchor={'height':1,'time':1776384120,'bits':self.anchor_bits,
                          'hash':'bc'*32,'previousblockhash':self.genesis['hash']}
        # Wire fixture tests serialization only; this is not a valid coinbase
        # and is never sent to a daemon or claimed to have consensus validity.
        self.template={'height':1,'version':1,'previousblockhash':'12'*32,
                       'utreexocommitment':'34'*32,'transactions':[],
                       'coinbasetxn':{'data':'ab00cd','txid':'56'*32}}

    def test_wire_and_real_hash_preserve_supplied_commitments(self):
        wire,digest=cycle.historical_template_block(self.template,self.genesis,self.anchor_bits,1_000_000)
        self.assertEqual(len(wire),133)
        self.assertEqual(wire[:4],bytes.fromhex('01000000'))
        self.assertEqual(wire[4:36],bytes.fromhex(self.template['previousblockhash'])[::-1])
        self.assertEqual(wire[36:68],bytes.fromhex(self.template['coinbasetxn']['txid'])[::-1])
        self.assertEqual(wire[68:100],bytes.fromhex(self.template['utreexocommitment'])[::-1])
        self.assertEqual(int.from_bytes(wire[100:108],'little'),1776384120)
        self.assertEqual(int.from_bytes(wire[108:112],'little'),0x207fffff)
        self.assertEqual(wire[116:128],bytes(12))
        self.assertEqual(wire[128:],bytes.fromhex('01ab00cd00'))
        self.assertEqual(hashlib.sha256(hashlib.sha256(wire[:128]).digest()).hexdigest(),digest)
        self.assertLessEqual(int(digest,16),0x7fffff << 232)

    def test_header_genesis_bits_cannot_substitute_for_mining_anchor(self):
        wire, _ = cycle.historical_template_block(self.template, self.genesis, self.anchor_bits, 0)
        self.assertEqual(int.from_bytes(wire[108:112], "little"), self.anchor_bits)
        self.assertNotEqual(self.genesis["bits"], self.anchor_bits)
        for wrong in (self.genesis["bits"], True, "207fffff", 0xffffffff):
            with self.subTest(anchor=wrong), self.assertRaises(AssertionError):
                cycle.historical_template_block(self.template, self.genesis, wrong, 0)
        with self.assertRaises(AssertionError):
            cycle.historical_template_block(self.template,
                dict(self.genesis, bits=self.anchor_bits), self.anchor_bits, 0)

    def test_disjoint_restart_nonce_ranges_produce_distinct_headers(self):
        one,one_hash=cycle.historical_template_block(self.template,self.genesis,self.anchor_bits,1_000_000)
        two,two_hash=cycle.historical_template_block(self.template,self.genesis,self.anchor_bits,2_000_000)
        self.assertNotEqual(one_hash,two_hash)
        self.assertEqual(one[:112]+one[116:],two[:112]+two[116:])
        self.assertTrue(1_000_000<=int.from_bytes(one[112:116],'little')<2_000_000)
        self.assertTrue(2_000_000<=int.from_bytes(two[112:116],'little')<3_000_000)

    def test_invalid_scope_refuses_before_mining(self):
        for height in (0,102,True,1.0):
            with self.subTest(height=height),self.assertRaises(AssertionError):
                cycle.historical_template_block(dict(self.template,height=height),self.genesis,self.anchor_bits,0)
        with self.assertRaises(AssertionError):
            cycle.historical_template_block(dict(self.template,transactions=[{}]),self.genesis,self.anchor_bits,0)
        with self.assertRaises(AssertionError):
            cycle.historical_template_block(self.template,dict(self.genesis,bits=0x1d00ffff),self.anchor_bits,0)
        with self.assertRaises(AssertionError):
            cycle.historical_template_block(self.template,self.genesis,self.anchor_bits,2**32)

    def test_last_historical_timestamp_is_exact_anchor_elapsed(self):
        wire,_=cycle.historical_template_block(dict(self.template,height=101),self.genesis,self.anchor_bits,0,
            time_anchor=self.time_anchor)
        self.assertEqual(int.from_bytes(wire[100:108],'little')-self.time_anchor['time'],101*120)
        self.assertEqual(self.time_anchor['time']-self.genesis['time'],120)

    def test_second_block_uses_block_one_time_and_zero_anchor_height(self):
        template=dict(self.template,height=2,previousblockhash=self.time_anchor['hash'])
        wire,digest=cycle.historical_template_block(template,self.genesis,self.anchor_bits,0,
            time_anchor=self.time_anchor)
        self.assertEqual(int.from_bytes(wire[100:108],'little'),1776384360)
        self.assertEqual(int.from_bytes(wire[100:108],'little')-self.time_anchor['time'],240)
        self.assertNotEqual(int.from_bytes(wire[100:108],'little'),self.genesis['time']+240)
        self.assertEqual(int.from_bytes(wire[108:112],'little'),self.anchor_bits)
        self.assertEqual(hashlib.sha256(hashlib.sha256(wire[:128]).digest()).hexdigest(),digest)
        self.assertLessEqual(int(digest,16),0x7fffff << 232)

    def test_missing_or_wrong_block_one_time_source_refuses(self):
        template=dict(self.template,height=2)
        for anchor in (None,self.genesis,dict(self.time_anchor,height=True),
                dict(self.time_anchor,time=self.genesis['time']),dict(self.time_anchor,time=1776384120.0),
                dict(self.time_anchor,bits=self.genesis['bits']),dict(self.time_anchor,hash='bad'),
                dict(self.time_anchor,previousblockhash='ff'*32)):
            with self.subTest(anchor=anchor),self.assertRaises(AssertionError):
                cycle.historical_template_block(template,self.genesis,self.anchor_bits,0,time_anchor=anchor)

    def test_node_binds_selected_block_one_before_submission(self):
        template=dict(self.template,height=2,previousblockhash=self.time_anchor['hash'])
        wire,digest=cycle.historical_template_block(template,self.genesis,self.anchor_bits,1_000_000,
            time_anchor=self.time_anchor)
        accepted={'hash':digest,'height':2,'time':1776384360,'bits':self.anchor_bits,
                  'nonce':int.from_bytes(wire[112:116],'little'),'previousblockhash':template['previousblockhash'],
                  'merkleroot':template['coinbasetxn']['txid'],'utreexo_root':template['utreexocommitment']}
        for wrong in (None,'hash','height','time'):
            with self.subTest(wrong=wrong),tempfile.TemporaryDirectory() as temporary:
                node=cycle.Node.__new__(cycle.Node);node.coupled_release=True;node.starts=1;node.root=Path(temporary)
                node.historical_pow_bits=self.anchor_bits;node.pow_profile='b'*64
                node.historical_genesis={k:self.genesis[k] for k in ('hash','time','bits')}
                source=self.time_anchor.copy()
                if wrong is not None:source[wrong]='ff'*32 if wrong=='hash' else 0
                node.rpc=Mock(side_effect=[1,self.genesis['hash'],self.genesis,self.time_anchor['hash'],source,
                    template,template['previousblockhash'],{},2,digest,accepted])
                record=node.root/'historical-proof-observations.jsonl'
                if wrong is not None:
                    with self.assertRaises(AssertionError):node.mine_historical('fixture-address')
                    self.assertNotIn('submitblock',[c.args[0] for c in node.rpc.call_args_list])
                    self.assertFalse(record.exists())
                else:
                    self.assertEqual(node.mine_historical('fixture-address'),digest)
                    result=json.loads(record.read_text())
                    self.assertEqual(result['asert_time_anchor_height'],1)
                    self.assertEqual(result['asert_time_anchor_hash'],self.time_anchor['hash'])
                    self.assertEqual(result['asert_time_anchor_time'],1776384120)
                    self.assertEqual(result['accepted'],accepted)
                    self.assertEqual(node.rpc.call_args_list[3].args,('getblockhash',[1]))
                    self.assertEqual(node.rpc.call_args_list[4].args,('getblockheader',[self.time_anchor['hash']]))
                    self.assertEqual(node.rpc.call_args_list[7].args,('submitblock',[wire.hex()]))

    def test_default_generate_retains_original_rpc_contract(self):
        node=cycle.Node.__new__(cycle.Node);node.coupled_release=False
        block='78'*32
        node.rpc=Mock(side_effect=[0,{'blocks':[block]},1,block])
        self.assertEqual(node.generate(1,'fixture-address'),[block])
        self.assertEqual(node.rpc.call_args_list[1].args,('generatetoaddress',[1,'fixture-address']))

    def test_coupled_generate_uses_historical_miner_and_retains_tip_checks(self):
        node=cycle.Node.__new__(cycle.Node);node.coupled_release=True
        block='78'*32
        node.rpc=Mock(side_effect=[100,101,block]);node.mine_historical=Mock(return_value=block)
        self.assertEqual(node.generate(1,'fixture-address'),[block])
        node.mine_historical.assert_called_once_with('fixture-address')
        self.assertNotIn('generatetoaddress',[call.args[0] for call in node.rpc.call_args_list])


    def test_node_checks_accepted_header_before_recording_public_evidence(self):
        wire, digest = cycle.historical_template_block(self.template, self.genesis, self.anchor_bits, 1_000_000)
        accepted = {"hash": digest, "height": 1, "time": self.genesis["time"] + 120,
                    "bits": self.anchor_bits, "nonce": int.from_bytes(wire[112:116], "little"),
                    "previousblockhash": self.template["previousblockhash"],
                    "merkleroot": self.template["coinbasetxn"]["txid"],
                    "utreexo_root": self.template["utreexocommitment"]}
        for changed in (None, "hash", "height", "time", "bits"):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temporary:
                node = cycle.Node.__new__(cycle.Node)
                node.coupled_release, node.starts, node.root = True, 1, Path(temporary)
                node.historical_pow_bits, node.pow_profile = self.anchor_bits, "b" * 64
                node.historical_genesis = {key: self.genesis[key] for key in ("hash", "time", "bits")}
                observed = accepted.copy()
                if changed is not None:
                    observed[changed] = "ff" * 32 if changed == "hash" else 0
                node.rpc = Mock(side_effect=[0, "aa" * 32, self.genesis,
                    self.template, self.template["previousblockhash"], {}, 1, digest, observed])
                record = node.root / "historical-proof-observations.jsonl"
                if changed is not None:
                    with self.assertRaises(AssertionError):
                        node.mine_historical("fixture-address")
                    self.assertFalse(record.exists())
                else:
                    self.assertEqual(node.mine_historical("fixture-address"), digest)
                    evidence = json.loads(record.read_text())
                    self.assertEqual(evidence["accepted"], accepted)
                    self.assertEqual(evidence["genesis_bits"], 0x1d31ffce)
                    self.assertEqual(evidence["anchor_bits"], 0x207fffff)
                    self.assertEqual(evidence["asert_time_anchor_height"], 0)
                    self.assertEqual(evidence["asert_time_anchor_hash"], self.genesis["hash"])
                    self.assertEqual(evidence["asert_time_anchor_time"], self.genesis["time"])
                    self.assertEqual(evidence["consensus_checksum"], node.pow_profile)
                    self.assertEqual(evidence["header_hex"], wire[:128].hex())
                    self.assertEqual(node.rpc.call_args_list[5].args, ("submitblock", [wire.hex()]))


class RpcPacingContract(unittest.TestCase):
    def setUp(self):
        HttpContract.setUp(self)
        self.now = 0.0
        self.sleeps = []
        self.sent = []

    def sleep(self, delay):
        self.sleeps.append(delay)
        self.now += delay

    def transport(self, request, timeout):
        self.assertEqual(timeout, 60)
        self.sent.append((self.now, json.loads(request.data)))
        return io.StringIO(json.dumps({"result": {}}))

    def test_serial_requests_and_restart_keep_monotonic_spacing(self):
        with patch.object(cycle.time, "monotonic", side_effect=lambda: self.now), \
             patch.object(cycle.time, "sleep", side_effect=self.sleep), \
             patch.object(cycle.urllib.request, "urlopen", side_effect=self.transport):
            for i in range(4):
                if i == 2:
                    self.node.starts += 1
                self.assertEqual(self.node.raw("submitblock", ["unit-wire"]), {})
        self.assertEqual(len(self.sent), 4)
        for i, (when, request) in enumerate(self.sent):
            self.assertAlmostEqual(when, i * 0.04)
            self.assertEqual(request, {"jsonrpc": "2.0", "id": 1,
                                      "method": "submitblock", "params": ["unit-wire"]})
        self.assertEqual(len(self.sleeps), 3)

    def test_slow_transport_creates_no_catchup_burst(self):
        def slow(request, timeout):
            result = self.transport(request, timeout)
            self.now += 0.2
            return result
        with patch.object(cycle.time, "monotonic", side_effect=lambda: self.now), \
             patch.object(cycle.time, "sleep", side_effect=self.sleep), \
             patch.object(cycle.urllib.request, "urlopen", side_effect=slow):
            for _ in range(3):
                self.node.raw("getblockcount")
        self.assertEqual(self.sleeps, [])
        for i, (when, _) in enumerate(self.sent):
            self.assertAlmostEqual(when, i * 0.2)
        self.assertAlmostEqual(self.node._rpc_not_before, 0.44)

    def test_early_wake_rechecks_deadline_before_send(self):
        def early(delay):
            self.sleeps.append(delay)
            self.now += delay / 4 if len(self.sleeps) == 1 else delay
        with patch.object(cycle.time, "monotonic", side_effect=lambda: self.now), \
             patch.object(cycle.time, "sleep", side_effect=early), \
             patch.object(cycle.urllib.request, "urlopen", side_effect=self.transport):
            self.node.raw("getblockcount")
            self.node.raw("getblockcount")
        self.assertEqual(len(self.sent), 2)
        self.assertEqual(len(self.sleeps), 2)
        self.assertAlmostEqual(self.sent[1][0], 0.04)

    def test_http_admission_error_propagates_without_retry(self):
        error = cycle.urllib.error.HTTPError("http://unit.invalid", 429,
                                            "Too Many Requests", {}, None)
        with patch.object(cycle.time, "monotonic", return_value=0.0), \
             patch.object(cycle.time, "sleep") as sleep, \
             patch.object(cycle.urllib.request, "urlopen", side_effect=error) as send:
            with self.assertRaises(cycle.urllib.error.HTTPError) as caught:
                self.node.raw("submitblock", ["unit-wire"])
        self.assertIs(caught.exception, error)
        send.assert_called_once()
        sleep.assert_not_called()
        self.assertFalse((self.node.root / "rpc-observations.jsonl").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
