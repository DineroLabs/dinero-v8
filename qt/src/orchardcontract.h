#pragma once
// The dinero-qt side of the wallet.orchard.* RPC contract, in one place.
// Every method name, request field and result field the Orchard screens use
// is here; screens never build or read Orchard JSON themselves. Mirrors
// src/rpc/orchard_account_rpc.cpp on Codex's integration branch (ac5c81f76).
// Candidate discovery extensions and qualification limits are documented in
// docs/design/orchard-qt-rpc-contract.md.
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>
#include <cmath>

namespace OrchardContract {

// --- Method names --------------------------------------------------------
inline const QString kCreateAccount = QStringLiteral("wallet.orchard.createaccount");
inline const QString kNewAddress = QStringLiteral("wallet.orchard.getnewaddress");
inline const QString kQueueShield = QStringLiteral("wallet.orchard.queueshield");
inline const QString kFinishShield = QStringLiteral("wallet.orchard.finishshield");
inline const QString kQueueSpend = QStringLiteral("wallet.orchard.queuespend");
inline const QString kFinishSpend = QStringLiteral("wallet.orchard.finishspend");
inline const QString kListAccounts = QStringLiteral("wallet.orchard.listaccounts");
inline const QString kListReceived = QStringLiteral("wallet.orchard.listreceived");
inline const QString kListOperations = QStringLiteral("wallet.orchard.listoperations");
// Reports authenticated checkpoint balances; it is not lasting spend readiness.
inline const QString kBalance = QStringLiteral("wallet.orchard.getbalance");

inline const QString kWalletBinding = QStringLiteral("wallet.orchard.getwalletbinding");
inline const QString kActivationStatus = QStringLiteral("orchard.getactivationstatus");
inline bool ValidBinding(const QString& s) {
    if (s.size() != 64) return false;
    for (const QChar c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return s != QString(64, '0');
}
inline std::optional<QString> ParseBinding(const QJsonValue& v, const QString& wallet) {
    if (!v.isObject()) return {};
    const auto o = v.toObject();
    if (o.contains("error") || o.value("wallet_name").toString() != wallet ||
        !o.value("wallet_binding").isString() || !ValidBinding(o.value("wallet_binding").toString())) return {};
    return o.value("wallet_binding").toString();
}

// --- Limits the node enforces (shown before sending) ---------------------
constexpr int kMaxShieldedPayments = 8;
constexpr int kMaxTransparentOutputs = 1024;
constexpr int kMaxMemoBytes = 512;
constexpr quint64 kMaxAccount = 0x7FFFFFFFULL;

// One nonzero 32-byte id per payment, from the system CSPRNG. Reusing it with
// byte-identical parameters is how a retry never creates a second payment.
inline QString NewRequestId() {
    QByteArray bytes(32, '\0');
    do {
        for (int i = 0; i < 32; i += 4) {
            const quint32 r = QRandomGenerator::system()->generate();
            for (int b = 0; b < 4; ++b) bytes[i + b] = char((r >> (8 * b)) & 0xff);
        }
    } while (bytes.count('\0') == 32);
    return QString::fromLatin1(bytes.toHex());
}

struct Recipient {
    QString address;
    quint64 amountUna = 0;
    QByteArray memo;  // shielded payments only; empty = no memo
};

// What one payment asks the node for. Kept unchanged across every retry.
struct Request {
    enum class Kind { Shield, Spend };
    Kind kind = Kind::Spend;
    quint64 account = 0;
    QString requestId;
    quint64 expectedRevision = 0;
    QVector<Recipient> payments;  // Orchard (dinorch1…) recipients
    QVector<Recipient> outputs;   // transparent recipients (unshield); Spend only
    quint64 feeUna = 0;
};

inline QJsonArray RecipientsJson(const QVector<Recipient>& list, bool shielded) {
    QJsonArray out;
    for (const auto& r : list) {
        QJsonObject o{{"address", r.address}, {"amount_una", QJsonValue(qint64(r.amountUna))}};
        if (shielded && !r.memo.isEmpty()) o["memo_hex"] = QString::fromLatin1(r.memo.toHex());
        out.append(o);
    }
    return out;
}

// Exact field sets: the node rejects any extra or missing field.
inline QJsonObject QueueParams(const Request& r) {
    QJsonObject p{{"account", QJsonValue(qint64(r.account))},
                  {"request_id", r.requestId},
                  {"expected_revision", QJsonValue(qint64(r.expectedRevision))},
                  {"payments", RecipientsJson(r.payments, true)},
                  {"fee_una", QJsonValue(qint64(r.feeUna))}};
    if (r.kind == Request::Kind::Spend) p["outputs"] = RecipientsJson(r.outputs, false);
    return p;
}
// Complete the authenticated stored request without resending recipients.
inline QJsonObject FinishParams(const Request& r) {
    return {{"account", QJsonValue(qint64(r.account))}, {"request_id", r.requestId}};
}
inline QString QueueMethod(const Request& r) { return r.kind == Request::Kind::Shield ? kQueueShield : kQueueSpend; }
inline QString FinishMethod(const Request& r) { return r.kind == Request::Kind::Shield ? kFinishShield : kFinishSpend; }

inline QJsonObject AccountParams(quint64 account) { return QJsonObject{{"account", QJsonValue(qint64(account))}}; }

// --- Results ---------------------------------------------------------------
// swap.*/wallet.orchard.* report refusals in-band as {"error": "..."}.
inline std::optional<QString> InBandError(const QJsonValue& v) {
    if (v.isObject() && v.toObject().contains("error") && !v.toObject().value("error").isNull())
        return v.toObject().value("error").toVariant().toString();
    return std::nullopt;
}

// Monetary values are accepted only when the JSON integer is exactly representable.
inline std::optional<quint64> ReadUna(const QJsonValue& v) {
    if (!v.isDouble()) return {};
    const double d=v.toDouble();
    if (!std::isfinite(d) || d<0 || d>9007199254740991.0 || std::floor(d)!=d) return {};
    return quint64(d);
}
// Wire hashes are lowercase 32-byte hex; zero is allowed for a checkpoint
// digest. Operation/request IDs separately require a nonzero value.
inline bool ValidHash32(const QJsonValue& value) {
    if (!value.isString()) return false;
    const auto s=value.toString();
    if (s.size()!=64) return false;
    for (const QChar c:s) if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) return false;
    return true;
}
struct CatalogAccount {
    quint64 account=0, revision=0, sequence=0, height=0;
    QString digest, blockHash;
};
struct AccountsReply {
    quint64 sourceSequence=0;
    QString sourceDigest;
    QVector<CatalogAccount> accounts;
};
inline std::optional<AccountsReply> ParseAccounts(const QJsonValue& v) {
    if(InBandError(v) || !v.isObject())return {};
    const auto o=v.toObject();const auto sequence=ReadUna(o.value("captured_source_sequence"));
    if(!sequence || !ValidHash32(o.value("captured_source_digest")) || !o.value("accounts").isArray())return {};
    const auto rows=o.value("accounts").toArray();if(rows.size()>1024)return {};
    AccountsReply result{*sequence,o.value("captured_source_digest").toString(),{}};QSet<quint64> ids;
    for(const auto& row:rows) {
        if(!row.isObject())return {};const auto a=row.toObject();
        const auto id=ReadUna(a.value("account")),revision=ReadUna(a.value("account_revision")),
            checkpoint=ReadUna(a.value("account_sequence")),height=ReadUna(a.value("checkpoint_height"));
        if(!id || *id>kMaxAccount || ids.contains(*id) || !revision || !*revision ||
           !checkpoint || *checkpoint>*sequence || !height || *height>0xffffffffULL ||
           !ValidHash32(a.value("account_digest")) || !ValidHash32(a.value("checkpoint_hash")) ||
           (*checkpoint==*sequence && a.value("account_digest")!=o.value("captured_source_digest")))return {};
        ids.insert(*id);result.accounts.push_back({*id,*revision,*checkpoint,*height,
            a.value("account_digest").toString(),a.value("checkpoint_hash").toString()});
    }
    return result;
}


// A page is one authenticated account revision; callers must never combine
// pages from different revisions. These receipts include spent notes/change.
struct ReceivedNote {
    QString txid, scope, blockHash, recipientHex, memoHex;
    quint64 action=0, amount=0, height=0;
};
struct ReceivedReply {
    quint64 account=0, revision=0, sequence=0, sourceSequence=0, height=0;
    quint64 offset=0, limit=0, total=0;
    QString digest, sourceDigest, blockHash;
    bool caughtUp=false;
    std::optional<quint64> nextOffset;
    QVector<ReceivedNote> notes;
};
inline bool ExactHex(const QJsonValue& v,int bytes) {
    if(!v.isString() || v.toString().size()!=bytes*2)return false;
    for(const QChar c:v.toString())if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    return true;
}
inline QJsonObject ReceivedParams(quint64 account,quint64 offset=0,quint64 revision=0) {
    auto p=AccountParams(account);p["offset"]=QJsonValue(qint64(offset));p["limit"]=100;
    if(revision)p["expected_revision"]=QJsonValue(qint64(revision));
    return p;
}
inline std::optional<ReceivedReply> ParseReceived(const QJsonValue& v) {
    if(!v.isObject() || InBandError(v))return {};
    const auto o=v.toObject();const auto account=ReadUna(o.value("account")),revision=ReadUna(o.value("account_revision")),
        seq=ReadUna(o.value("account_sequence")),source=ReadUna(o.value("captured_source_sequence")),height=ReadUna(o.value("checkpoint_height")),
        offset=ReadUna(o.value("offset")),limit=ReadUna(o.value("limit")),total=ReadUna(o.value("total_count"));
    if(!account || *account>kMaxAccount || !revision || !*revision || !seq || !source || *seq>*source ||
       !height || *height>0xffffffffULL || !offset || !limit || !*limit || *limit>1000 || !total || *total>65536 || *offset>*total ||
       !ValidHash32(o.value("account_digest")) || !ValidHash32(o.value("captured_source_digest")) || !ValidHash32(o.value("checkpoint_hash")) ||
       !o.value("history_complete").isBool() || !o.value("history_complete").toBool() ||
       !o.value("account_caught_up_to_captured_source").isBool() || !o.value("received").isArray() || !o.contains("next_offset"))return {};
    const bool caughtUp=*seq==*source && o.value("account_digest")==o.value("captured_source_digest");
    if((*seq==*source && !caughtUp) || o.value("account_caught_up_to_captured_source").toBool()!=caughtUp)return {};
    const auto rows=o.value("received").toArray();const quint64 count=qMin(*limit,*total-*offset);
    if(quint64(rows.size())!=count)return {};
    ReceivedReply r;r.account=*account;r.revision=*revision;r.sequence=*seq;r.sourceSequence=*source;r.height=*height;
    r.offset=*offset;r.limit=*limit;r.total=*total;r.caughtUp=caughtUp;r.digest=o.value("account_digest").toString();
    r.sourceDigest=o.value("captured_source_digest").toString();r.blockHash=o.value("checkpoint_hash").toString();
    if(*offset+count<*total) {
        r.nextOffset=ReadUna(o.value("next_offset"));if(!r.nextOffset || *r.nextOffset!=*offset+count)return {};
    } else if(!o.value("next_offset").isNull())return {};
    QSet<QString> ids;quint64 priorHeight=0;QString priorBlock;
    for(const auto& row:rows) {
        if(!row.isObject())return {};const auto n=row.toObject();
        const auto action=ReadUna(n.value("action_index")),amount=ReadUna(n.value("amount_una")),at=ReadUna(n.value("height"));
        const auto scope=n.value("scope").toString(),txid=n.value("txid").toString(),block=n.value("block_hash").toString();
        if(!action || *action>0xffffffffULL || !amount || !*amount || !at || *at>*height || *at<priorHeight ||
           (scope!="external" && scope!="internal") || !ValidBinding(txid) || !ValidBinding(block) ||
           !ExactHex(n.value("recipient_hex"),43) || !ExactHex(n.value("memo_hex"),512) ||
           (*at==*height && block!=r.blockHash) ||
           (!priorBlock.isEmpty() && *at==priorHeight && block!=priorBlock))return {};
        const auto id=txid+":"+QString::number(*action);if(ids.contains(id))return {};ids.insert(id);priorHeight=*at;priorBlock=block;
        r.notes.push_back({txid,scope,block,n.value("recipient_hex").toString(),n.value("memo_hex").toString(),*action,*amount,*at});
    }
    return r;
}

struct AccountReply { QString address; quint64 account = 0; quint64 revision = 0; bool requiresSync = false; };
inline std::optional<AccountReply> ParseAccount(const QJsonValue& v, bool creating=false) {
    if (InBandError(v) || !v.isObject()) return {};
    const auto o=v.toObject();const auto account=ReadUna(o.value("account")), revision=ReadUna(o.value("revision"));
    const auto address=o.value("address").toString();
    if (!account || *account>kMaxAccount || !revision || *revision==0 ||
        !o.value("address").isString() || address.isEmpty() || address.trimmed()!=address ||
        (o.contains("requires_sync") && !o.value("requires_sync").isBool()) ||
        (creating && (!o.value("requires_sync").isBool() || !o.value("requires_sync").toBool()))) return {};
    return AccountReply{address,*account,*revision,o.value("requires_sync").toBool()};
}

struct ActivationReply {
    QString network, state;
    quint64 tipHeight=0, nextHeight=0;
    bool activeAtTip=false, activeForNext=false, walletBackend=false;
};
inline QString CanonicalNetwork(const QString& name) {
    if(name=="main" || name=="mainnet")return "mainnet";
    if(name=="test" || name=="testnet")return "testnet";
    if(name=="regtest")return "regtest";
    return {};
}
inline std::optional<ActivationReply> ParseActivation(const QJsonValue& v,const QString& expectedNetwork) {
    if(InBandError(v) || !v.isObject())return {};
    const auto o=v.toObject();const auto network=o.value("network").toString(), state=o.value("activation_state").toString();
    const auto tip=ReadUna(o.value("tip_height")), next=ReadUna(o.value("next_block_height"));
    const auto mode=o.value("storage_mode").toString();
    if(CanonicalNetwork(expectedNetwork).isEmpty() || network!=CanonicalNetwork(expectedNetwork) ||
        !tip || *tip>0xffffffffULL || !next || *next!=*tip+1 || !ValidHash32(o.value("tip_hash")) ||
        !o.value("rule_active_at_tip").isBool() || !o.value("rule_active_for_next_block").isBool() ||
        !o.value("wallet_backend_compiled").isBool() || (mode!="full" && mode!="compact"))return {};
    const bool active=o.value("rule_active_at_tip").toBool(), nextActive=o.value("rule_active_for_next_block").toBool();
    if(state=="unscheduled") {
        if(!o.contains("activation_height") || !o.value("activation_height").isNull() ||
            !o.contains("branch_id") || !o.value("branch_id").isNull() || active || nextActive)return {};
    } else if(state=="scheduled" || state=="active") {
        const auto height=ReadUna(o.value("activation_height")), branch=ReadUna(o.value("branch_id"));
        if(!height || *height>=0xffffffffULL || !branch || *branch==0 || *branch>0xffffffffULL ||
            active!=(*tip>=*height) || nextActive!=(*next>=*height) || (state=="active")!=active)return {};
    } else return {};
    return ActivationReply{network,state,*tip,*next,active,nextActive,o.value("wallet_backend_compiled").toBool()};
}

struct QueueReply {
    QString operationId, durableState;
    quint64 accountRevision = 0;
    bool proofQueued = false, existingRequest = false, archived = false;
    quint64 account = 0;
};
inline std::optional<QueueReply> ParseQueue(const QJsonValue& v) {
    if (InBandError(v) || !v.isObject()) return {};
    const auto o=v.toObject();
    const auto account=ReadUna(o.value("account")), revision=ReadUna(o.value("account_revision"));
    const auto state=o.value("durable_state").toString();
    if (!account || *account>kMaxAccount || !revision ||
        !o.value("operation_id").isString() || !ValidBinding(o.value("operation_id").toString()) ||
        (state!="reserved" && state!="signed") || !o.value("proof_queued").isBool() ||
        !o.value("existing_request").isBool() || !o.value("archived").isBool()) return {};
    return QueueReply{o.value("operation_id").toString(),state,*revision,
        o.value("proof_queued").toBool(),o.value("existing_request").toBool(),o.value("archived").toBool(),*account};
}

struct FinishReply {
    QString operationId, txid, submissionCode, submissionMessage;
    bool admitted = false, alreadyInMempool = false;
    quint64 account = 0;
};
inline std::optional<FinishReply> ParseFinish(const QJsonValue& v) {
    if (InBandError(v) || !v.isObject()) return {};
    const auto o=v.toObject();const auto account=ReadUna(o.value("account"));
    if (!account || *account>kMaxAccount || !o.value("operation_id").isString() ||
        !ValidBinding(o.value("operation_id").toString()) || o.value("durable_state").toString()!="signed" ||
        !ValidHash32(o.value("txid")) || !o.value("admitted").isBool() ||
        !o.value("already_in_mempool").isBool() || !o.value("submission_code").isString() ||
        o.value("submission_code").toString().isEmpty() || !o.value("submission_message").isString()) return {};
    // Spend completion reports this field; shield completion currently does not.
    if (o.contains("account_revision") && !ReadUna(o.value("account_revision"))) return {};
    return FinishReply{o.value("operation_id").toString(),o.value("txid").toString(),
        o.value("submission_code").toString(),o.value("submission_message").toString(),
        o.value("admitted").toBool(),o.value("already_in_mempool").toBool(),*account};
}

// Only queued/running is progress. Missing/failed/cancelled must not spin.
inline bool IsProofNotReady(const QJsonValue& reply) {
    if (!reply.isObject()) return false;
    const auto o=reply.toObject();const auto state=o.value("proof_state").toString();
    return o.value("error_code").toString()=="proof_not_ready" &&
        o.value("reservation_retained").isBool() && o.value("reservation_retained").toBool() &&
        (state=="queued" || state=="running");
}
// The node reports stale_account_revision, request_id_conflict and
// request_not_current separately. These refusals do not authorize a new ID
// or imply that a reservation was released.

struct Operation {
    QString operationId, durableState, txid;
    QString outcome;  // empty means no recorded observation at this checkpoint
    quint64 height = 0;
    QString completionMethod; // absent for old intents; never infer it
};
struct OperationsReply {
    quint64 accountRevision = 0, accountSequence = 0, sourceSequence = 0;
    QVector<Operation> operations;
    quint64 account = 0;
    bool syncing() const { return accountSequence < sourceSequence; }
};
inline std::optional<OperationsReply> ParseOperations(const QJsonValue& v) {
    if (InBandError(v) || !v.isObject()) return {};
    const auto o=v.toObject();
    const auto account=ReadUna(o.value("account")), revision=ReadUna(o.value("account_revision")),
        sequence=ReadUna(o.value("account_sequence")), source=ReadUna(o.value("captured_source_sequence"));
    if (!account || *account>kMaxAccount || !revision || !sequence || !source || *sequence>*source ||
        !ValidHash32(o.value("account_digest")) || !ValidHash32(o.value("captured_source_digest")) ||
        (*sequence==*source && o.value("account_digest")!=o.value("captured_source_digest")) ||
        !o.value("operations").isArray()) return {};
    OperationsReply r;
    r.account=*account;r.accountRevision=*revision;r.accountSequence=*sequence;r.sourceSequence=*source;
    QSet<QString> seen;
    // Validate every row before returning a result; a malformed late row must
    // not publish an apparently complete prefix or an invented empty list.
    for (const auto& value:o.value("operations").toArray()) {
        if (!value.isObject()) return {};
        const auto op=value.toObject();
        if (!op.value("operation_id").isString() || !ValidBinding(op.value("operation_id").toString()) ||
            seen.contains(op.value("operation_id").toString()) || !op.value("durable_state").isString() ||
            !op.contains("chain_observation")) return {};
        const auto phase=op.value("durable_state").toString();
        if (phase!="reserved" && phase!="signed") return {};
        if (phase=="signed" ? !ValidHash32(op.value("txid")) : op.contains("txid")) return {};
        Operation x{op.value("operation_id").toString(),phase,
            phase=="signed"?op.value("txid").toString():QString(),{},0};
        if(op.contains("completion_method")) {
            if(!op.value("completion_method").isString())return {};
            x.completionMethod=op.value("completion_method").toString();
            if(x.completionMethod!=kFinishShield && x.completionMethod!=kFinishSpend)return {};
        }
        seen.insert(x.operationId);
        const auto obs=op.value("chain_observation");
        if (!obs.isNull()) {
            if (!obs.isObject()) return {};
            const auto observation=obs.toObject();
            const auto height=ReadUna(observation.value("height"));
            const auto outcome=observation.value("outcome").toString();
            if (!height || *height>0xffffffffULL ||
                (outcome!="confirmed" && outcome!="conflicted") ||
                !ValidHash32(observation.value("block_hash")) ||
                !ValidHash32(observation.value("transaction_id"))) return {};
            if (outcome=="confirmed" && (phase!="signed" ||
                observation.value("transaction_id").toString()!=x.txid)) return {};
            x.outcome=outcome;x.height=*height;
            // A conflicting transaction belongs to the observation, not to
            // this payment. Never use it as the operation's own transaction ID.
        }
        r.operations.append(x);
    }
    return r;
}

struct BalanceReply {
    quint64 confirmed=0, reserved=0, unreserved=0;
    bool caughtUp=false;
    quint64 account=0, accountRevision=0, accountSequence=0, sourceSequence=0;
};
inline std::optional<BalanceReply> ParseBalance(const QJsonValue& v) {
    if (InBandError(v) || !v.isObject()) return {};
    const auto o=v.toObject();
    const auto account=ReadUna(o.value("account")), revision=ReadUna(o.value("account_revision")),
        sequence=ReadUna(o.value("account_sequence")), source=ReadUna(o.value("captured_source_sequence")),
        height=ReadUna(o.value("checkpoint_height"));
    const auto confirmed=ReadUna(o.value("confirmed_una")), reserved=ReadUna(o.value("reserved_confirmed_una")),
        unreserved=ReadUna(o.value("unreserved_confirmed_una"));
    if (!account || *account>kMaxAccount || !revision || *revision==0 || !sequence || !source || *sequence>*source ||
        !height || *height>0xffffffffULL || !ValidHash32(o.value("checkpoint_hash")) ||
        !ValidHash32(o.value("account_digest")) || !ValidHash32(o.value("captured_source_digest")) ||
        (*sequence==*source && o.value("account_digest")!=o.value("captured_source_digest")) ||
        !confirmed || !reserved || !unreserved || *reserved>*confirmed ||
        *unreserved!=*confirmed-*reserved || !o.value("account_caught_up_to_captured_source").isBool() ||
        o.value("account_caught_up_to_captured_source").toBool()!=(*sequence==*source)) return {};
    return BalanceReply{*confirmed,*reserved,*unreserved,o.value("account_caught_up_to_captured_source").toBool(),
        *account,*revision,*sequence,*source};
}

}  // namespace OrchardContract
