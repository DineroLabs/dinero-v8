#pragma once
// Orchard payment flow for dinero-qt: no widgets, no network. One OrchardPayment
// follows one request (shield, send or unshield) through the node's two-step
// queue/finish protocol. A finished proof is NOT a successful payment: success
// is only "submitted" (accepted by the node's mempool) and then "confirmed"
// (seen in a block by the account's last synchronized checkpoint).
#include "orchardcontract.h"

#include <QString>

#include <optional>
#include <stdexcept>

namespace OrchardFlow {

enum class State {
    Draft,        // not sent to the node yet
    Queued,       // the node holds the request (reserved); proof not built yet
    Proving,      // the node is building the proof
    Signed,       // proof built and transaction signed; NOT sent / not paid yet
    Submitted,    // the node accepted it into its mempool; not confirmed yet
    Rejected,     // refused on submission, or conflicted on chain
    Confirmed,    // seen in a block by the account's synchronized checkpoint
    Archived,     // authenticated request is no longer current; never resubmit
    NeedsRetry,   // a call failed or the reply was unclear; retry is safe
};

inline QString Label(State s) {
    switch (s) {
        case State::Draft: return "Not sent";
        case State::Queued: return "Queued";
        case State::Proving: return "Building proof";
        case State::Signed: return "Proof ready — not sent yet";
        case State::Submitted: return "Submitted — waiting for a block";
        case State::Rejected: return "Rejected";
        case State::Confirmed: return "Confirmed";
        case State::Archived: return "Archived — not resubmitted";
        case State::NeedsRetry: return "Needs retry";
    }
    return {};
}
inline bool IsFinal(State s) { return s == State::Confirmed || s == State::Archived; }

class OrchardPayment {
public:
    explicit OrchardPayment(OrchardContract::Request request)
        : request_(std::move(request)), account_(request_->account), requestId_(request_->requestId),
          completionMethod_(OrchardContract::FinishMethod(*request_)) {}

    // A recovered selector has no fabricated recipient list, fee or queue
    // request. Only an explicit user action may construct this resume flow.
    static std::optional<OrchardPayment> Resume(quint64 account,
            const OrchardContract::Operation& op,quint64 revision) {
        if(account>OrchardContract::kMaxAccount || revision==0 ||
           !OrchardContract::ValidBinding(op.operationId) || !op.outcome.isEmpty() ||
           (op.completionMethod!=OrchardContract::kFinishShield && op.completionMethod!=OrchardContract::kFinishSpend) ||
           (op.durableState!="reserved" && op.durableState!="signed") ||
           (op.durableState=="signed" ? !OrchardContract::ValidHash32(QJsonValue(op.txid)) : !op.txid.isEmpty()))return {};
        OrchardPayment out(account,op.operationId,op.completionMethod);
        out.operationId_=op.operationId;out.txid_=op.txid;out.observedRevision_=revision;
        out.state_=op.durableState=="signed"?State::Signed:State::Queued;
        return out;
    }
    bool canQueue() const { return request_.has_value(); }
    QString requestId() const { return requestId_; }
    QString finishMethod() const { return completionMethod_; }
    QJsonObject finishParams() const {
        return {{"account",QJsonValue(qint64(account_))},{"request_id",requestId_}};
    }

    // The request sent on every attempt: same id, same recipients, same fee.
    const OrchardContract::Request& request() const {
        if(!request_)throw std::logic_error("Stored completion has no queue intent");
        return *request_;
    }
    State state() const { return state_; }
    QString detail() const { return detail_; }
    QString txid() const { return txid_; }
    QString operationId() const { return operationId_; }
    bool requiresExplicitRetry() const { return requiresExplicitRetry_; }
    bool canRetry() const { return !chainConflict_ && (state_==State::NeedsRetry || state_==State::Rejected ||
        (requiresExplicitRetry_ && (state_==State::Signed || state_==State::Queued))); }
    // Only a deliberate Retry click authorizes a read-discovered saved state
    // to be submitted again. A poll must never grant that authority.
    bool authorizeRetry() {
        if(!canRetry())return false;
        requiresExplicitRetry_=false;
        return true;
    }

    // A newer account revision may be adopted on retry; nothing else changes.
    void adoptRevision(quint64 revision) {
        if (request_ && !IsFinal(state_) && revision > request_->expectedRevision) request_->expectedRevision = revision;
    }

    void onQueueReply(const QJsonValue& v) {
        if(!canQueue())return retry("Stored payment cannot be queued as a new request");
        if (IsFinal(state_)) return;
        if (const auto e = OrchardContract::InBandError(v)) return retry(*e);
        const auto r = OrchardContract::ParseQueue(v);
        if (!r) return retry("Unexpected reply from the node");
        if (r->account!=account_ || r->operationId!=requestId_)
            return retry("Reply does not match this payment");
        operationId_ = r->operationId;
        if (r->archived) { state_ = State::Archived; detail_ = "This request was already completed or abandoned"; return; }
        if (r->durableState == "signed") { state_ = State::Signed; detail_.clear(); return; }
        if (r->durableState == "reserved") { state_ = State::Queued; detail_.clear(); return; }
        retry("Unknown request state: " + r->durableState);
    }

    void onFinishReply(const QJsonValue& v) {
        if (IsFinal(state_)) return;
        if (const auto e = OrchardContract::InBandError(v)) {
            if (OrchardContract::IsProofNotReady(v)) { state_ = State::Proving; detail_.clear(); return; }
            return retry(*e);
        }
        const auto r = OrchardContract::ParseFinish(v);
        if (!r) return retry("Unexpected reply from the node");
        if (r->account!=account_ || r->operationId!=requestId_ ||
            (!txid_.isEmpty() && txid_!=r->txid))
            return retry("Reply does not match this payment");
        txid_ = r->txid;
        if (!r->operationId.isEmpty()) operationId_ = r->operationId;
        if (r->admitted || r->alreadyInMempool) { state_ = State::Submitted; detail_.clear(); return; }
        // Signed but refused by the mempool: the payment did NOT happen.
        state_ = State::Rejected;
        detail_ = r->submissionMessage.isEmpty() ? r->submissionCode : r->submissionMessage;
    }

    // The account checkpoint may lag the chain; only its observations count.
    void onOperations(const OrchardContract::OperationsReply& ops) {
        // Account revisions, unlike chain heights, advance across reorg writes.
        // Ignore older asynchronous observations, but permit a newer checkpoint
        // to withdraw a prior confirmation or conflict.
        if (ops.account!=account_ || ops.accountRevision<observedRevision_) return;
        observedRevision_=ops.accountRevision;
        for (const auto& op : ops.operations) {
            const bool mine = !operationId_.isEmpty() ? op.operationId==operationId_
                : (!txid_.isEmpty() && op.txid==txid_);
            if (!mine) continue;
            const bool rolledBack=state_==State::Confirmed || chainConflict_ || state_==State::Archived;
            if (op.outcome=="confirmed") {
                state_=State::Confirmed;chainConflict_=false;detail_=QString("Block %1 at the wallet checkpoint").arg(op.height);
            } else if (op.outcome=="conflicted") {
                state_=State::Rejected;chainConflict_=true;detail_="Conflicted at the wallet checkpoint";
            } else if (rolledBack || state_==State::Draft || state_==State::NeedsRetry) {
                chainConflict_=false;
                requiresExplicitRetry_=true;
                detail_="Check this saved payment and choose Retry before submitting it again.";
                if(op.durableState=="signed") state_=State::Signed;
                else if(op.durableState=="reserved") state_=State::Queued;
            }
            return;
        }
        // Absence alone does not prove completion, cancellation or release.
    }

private:
    void retry(const QString& why) { state_ = State::NeedsRetry; detail_ = why; }

    OrchardPayment(quint64 account,QString id,QString method)
        : account_(account),requestId_(std::move(id)),completionMethod_(std::move(method)) {}
    std::optional<OrchardContract::Request> request_;
    quint64 account_=0;
    QString requestId_,completionMethod_;
    State state_ = State::Draft;
    quint64 observedRevision_ = 0;
    bool chainConflict_ = false;
    bool requiresExplicitRetry_ = false;
    QString detail_, txid_, operationId_;
};

// Replies are routed by tag. A reply carries the wallet and generation it was
// requested for; anything else (a previously selected wallet) is dropped.
struct ReplyTag {
    QString method;  // short name, e.g. "queue"
    QString wallet;
    quint64 generation = 0;
    QString requestId;
    static QString Make(const QString& method, const QString& wallet, quint64 generation, const QString& requestId={}) {
        const auto base=QString("orchardtab|%1|%2|%3").arg(method,QString::fromLatin1(wallet.toUtf8().toHex())).arg(generation);
        return requestId.isEmpty()?base:base+"|"+requestId;
    }
    static std::optional<ReplyTag> Parse(const QString& tag) {
        const auto parts = tag.split('|');
        if ((parts.size() != 4 && parts.size()!=5) || parts[0] != "orchardtab") return std::nullopt;
        bool ok = false;
        const quint64 gen = parts[3].toULongLong(&ok);
        if (!ok) return std::nullopt;
        const auto id=parts.size()==5?parts[4]:QString();
        if(parts.size()==5 && !OrchardContract::ValidBinding(id)) return {};
        return ReplyTag{parts[1], QString::fromUtf8(QByteArray::fromHex(parts[2].toLatin1())), gen,id};
    }
};

// Exact DIN text -> una (1 DIN = 100,000,000 una). No floating point.
inline std::optional<quint64> ParseDin(const QString& text) {
    const QString t = text.trimmed();
    if (t.isEmpty() || t.count('.') > 1) return std::nullopt;
    const int dot = t.indexOf('.');
    const QString whole = dot < 0 ? t : t.left(dot);
    const QString frac = dot < 0 ? QString() : t.mid(dot + 1);
    if (whole.isEmpty() && frac.isEmpty()) return std::nullopt;
    if (frac.size() > 8) return std::nullopt;
    for (const QChar c : whole + frac) if (!c.isDigit()) return std::nullopt;
    if (whole.size() > 11) return std::nullopt;  // far above total supply; avoids overflow
    const quint64 w = whole.isEmpty() ? 0 : whole.toULongLong();
    const quint64 f = frac.isEmpty() ? 0 : QString(frac).leftJustified(8, '0').toULongLong();
    const quint64 una = w * 100000000ULL + f;
    if (una == 0) return std::nullopt;
    return una;
}

inline QString FormatDin(quint64 una) {
    return QString("%1.%2").arg(una / 100000000ULL).arg(una % 100000000ULL, 8, 10, QChar('0'));
}

// Orchard receivers are dinorch1… (mainnet), tdinorch1… (testnet), rdinorch1… (regtest).
inline QString OrchardHrp(const QString& chain) {
    const QString c = chain.toLower();
    if (c == "main" || c == "mainnet") return "dinorch";
    if (c == "test" || c == "testnet") return "tdinorch";
    return "rdinorch";
}
inline bool LooksLikeOrchardAddress(const QString& address, const QString& chain) {
    return address.trimmed().toLower().startsWith(OrchardHrp(chain) + "1");
}
inline QString TransparentHrp(const QString& chain) {
    const QString c = chain.toLower();
    if (c == "main" || c == "mainnet") return "din";
    if (c == "test" || c == "testnet") return "tdin";
    return "rdin";
}
inline bool LooksLikeTransparentAddress(const QString& address, const QString& chain) {
    const QString a = address.trimmed().toLower();
    return a.startsWith(TransparentHrp(chain) + "1") && !LooksLikeOrchardAddress(address, chain);
}

}  // namespace OrchardFlow
