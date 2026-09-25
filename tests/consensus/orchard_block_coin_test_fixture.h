#pragma once
#include "orchard_block_test_fixture.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_validation.h"

static Transaction Child(const OutPoint& point,const UTXOEntry& coin,const Fixture& keys,uint64_t fee=123) {
    Transaction tx;tx.version=2;tx.witness_version=1;
    TxInput input;input.prevout.txid=point.txid;input.prevout.vout=point.vout;input.sequence=UINT32_MAX;
    tx.vin.push_back(input);
    tx.vout.emplace_back(AmountUna::Una(coin.value.GetUna()-fee),coin.scriptPubKey);
    ScriptExecutionContext context(&tx,0,coin.value.GetUna(),SCRIPT_VERIFY_STANDARD,
        {coin.value.GetUna()},{coin.scriptPubKey},{0},{{}});
    const auto digest=SignatureHashTaproot(context,0,{});
    Require(digest.size()==32);
    secp256k1_keypair pair;Require(secp256k1_keypair_create(keys.ctx.get(),&pair,keys.secret1.data()));
    Bytes signature(64);Hash aux{};
    Require(secp256k1_schnorrsig_sign32(keys.ctx.get(),signature.data(),digest.data(),&pair,aux.data()));
    tx.vin[0].witness={signature};
    Require(ValidateSpend(tx,0,coin,20001,{coin})==ScriptValidationResult::OK);
    return tx;
}
static Bytes Wire(const Transaction& tx) {return tx.Serialize(TxSerializationMode::WithWitness);}
