// Copyright (c) 2026 Dinero Labs.
//
// JSON-line file persistence for the vault ledger.
//
// Format: one entry per line, sorted-keys JSON without trailing
// whitespace. Variant tag goes in field "kind"; remaining fields
// are concrete-shape-dependent.

#include "vault/ledger_store.h"

#include <fstream>
#include <charconv>
#include <set>
#include <type_traits>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dinero::vault {

namespace {

// ----- minimal JSON helpers -----
//
// We avoid pulling in a full JSON library (the project's `Json` is
// daemon-side and tied to the RPC layer). The vault store's needs
// are tiny — sorted-keys integer/string emit + a hand-rolled
// tokeniser on the read path.

std::string escapeString(const std::string& s) {
    std::ostringstream oss;
    oss << '"';
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    oss << "\\u" << std::setw(4) << std::setfill('0') << std::hex
                        << static_cast<int>(c);
                } else {
                    oss << c;
                }
        }
    }
    oss << '"';
    return oss.str();
}

template<size_t N>
std::string hexEncode(const std::array<uint8_t, N>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

template<size_t N>
bool hexDecode(const std::string& hex, std::array<uint8_t, N>& out) {
    if(hex.size()!=2*N)return false;
    const auto nibble=[](char c)->int {
        if(c>='0' && c<='9')return c-'0';
        if(c>='a' && c<='f')return c-'a'+10;
        if(c>='A' && c<='F')return c-'A'+10;
        return -1;
    };
    std::array<uint8_t,N> candidate{};
    for(size_t i=0;i<candidate.size();++i) {
        const int hi=nibble(hex[2*i]),lo=nibble(hex[2*i+1]);
        if(hi<0 || lo<0)return false;
        candidate[i]=static_cast<uint8_t>((hi<<4)|lo);
    }
    out=candidate;return true;
}

std::string outpointJson(const OutpointId& op) {
    std::ostringstream oss;
    oss << "{\"txid\":" << escapeString(hexEncode(op.txid_raw)) << ",\"vout\":" << op.vout << "}";
    return oss.str();
}

std::string serializeEntry(const LedgerEntry& entry) {
    std::ostringstream oss;
    std::visit(
        [&](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T,WithdrawalAllocationReserved> ||
                          std::is_same_v<T,WithdrawalAllocationDispatchStarted> ||
                          std::is_same_v<T,WithdrawalAllocationPaymentBound> ||
                          std::is_same_v<T,WithdrawalAllocationIncluded> ||
                          std::is_same_v<T,WithdrawalAllocationDisconnected> ||
                          std::is_same_v<T,WithdrawalAllocationReleased>) {
                const char* kind=nullptr;
                if constexpr(std::is_same_v<T,WithdrawalAllocationReserved>)kind="withdrawalAllocationReserved";
                else if constexpr(std::is_same_v<T,WithdrawalAllocationDispatchStarted>)kind="withdrawalAllocationDispatchStarted";
                else if constexpr(std::is_same_v<T,WithdrawalAllocationPaymentBound>)kind="withdrawalAllocationPaymentBound";
                else if constexpr(std::is_same_v<T,WithdrawalAllocationIncluded>)kind="withdrawalAllocationIncluded";
                else if constexpr(std::is_same_v<T,WithdrawalAllocationDisconnected>)kind="withdrawalAllocationDisconnected";
                else kind="withdrawalAllocationReleased";
                oss << "{\"account\":" << escapeString(concrete.account.raw);
                if constexpr(std::is_same_v<T,WithdrawalAllocationReserved>)oss << ",\"amount\":" << concrete.amount;
                oss << ",\"at\":" << concrete.at;
                if constexpr(std::is_same_v<T,WithdrawalAllocationPaymentBound>)
                    oss << ",\"backend\":" << escapeString(concrete.backend.raw)
                        << ",\"bodyHash\":" << escapeString(hexEncode(concrete.payment.body_hash));
                if constexpr(std::is_same_v<T,WithdrawalAllocationIncluded> || std::is_same_v<T,WithdrawalAllocationDisconnected>)
                    oss << ",\"blockHash\":" << escapeString(hexEncode(concrete.inclusion.block_hash))
                        << ",\"height\":" << concrete.inclusion.height;
                oss << ",\"kind\":" << escapeString(kind);
                if constexpr(std::is_same_v<T,WithdrawalAllocationPaymentBound>)
                    oss << ",\"output\":" << outpointJson(concrete.payment.output);
                oss << ",\"requestId\":" << escapeString(hexEncode(concrete.request)) << ",\"seq\":" << concrete.seq;
                if constexpr(std::is_same_v<T,WithdrawalAllocationReserved>) {
                    oss << ",\"sources\":[";
                    for(size_t i=0;i<concrete.sources.size();++i) {
                        if(i)oss << ',';
                        oss << "{\"amount\":" << concrete.sources[i].amount
                            << ",\"creditSeq\":" << concrete.sources[i].credit_seq << '}';
                    }
                    oss << ']';
                }
                oss << '}';
            } else if constexpr(std::is_same_v<T,CreditPositionMatured> ||
                                std::is_same_v<T,CreditPositionReverted> || std::is_same_v<T,CreditPositionRestored>) {
                const char* kind=std::is_same_v<T,CreditPositionMatured>?"creditPositionMatured":
                    std::is_same_v<T,CreditPositionReverted>?"creditPositionReverted":"creditPositionRestored";
                oss << "{\"account\":" << escapeString(concrete.account.raw) << ",\"at\":" << concrete.at
                    << ",\"creditSeq\":" << concrete.credit_seq << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":" << escapeString(kind) << ",\"seq\":" << concrete.seq << '}';
            } else if constexpr (std::is_same_v<T, DepositObserved>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"amount\":" << concrete.amount << ",\"at\":" << concrete.at
                    << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"depositObserved\"" << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, CreditOpened>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"amount\":" << concrete.amount << ",\"at\":" << concrete.at
                    << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"creditOpened\"" << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, CreditSettled>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"at\":" << concrete.at << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"creditSettled\"" << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, CreditReverted>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"at\":" << concrete.at << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"creditReverted\"" << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, CreditReinstated>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"at\":" << concrete.at << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"creditReinstated\""
                    << ",\"reversalSeq\":" << concrete.reversalSeq
                    << ",\"compensationSeq\":" << concrete.compensationSeq
                    << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, WithdrawalInitiated>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"amount\":" << concrete.amount << ",\"at\":" << concrete.at
                    << ",\"backend\":" << escapeString(concrete.backend.raw)
                    << ",\"kind\":\"withdrawalInitiated\""
                    << ",\"request\":" << outpointJson(concrete.request)
                    << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, WithdrawalSettled>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"at\":" << concrete.at << ",\"kind\":\"withdrawalSettled\""
                    << ",\"request\":" << outpointJson(concrete.request)
                    << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, WithdrawalReverted>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"at\":" << concrete.at << ",\"kind\":\"withdrawalReverted\""
                    << ",\"request\":" << outpointJson(concrete.request)
                    << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, CompensatingDebit>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"amount\":" << concrete.amount << ",\"at\":" << concrete.at
                    << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"compensatingDebit\""
                    << ",\"operatorLoss\":" << concrete.operatorLoss
                    << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, PolicyAdjustment>) {
                oss << "{";
                oss << "\"account\":";
                if (concrete.account.has_value()) {
                    oss << escapeString(concrete.account->raw);
                } else {
                    oss << "null";
                }
                oss << ",\"at\":" << concrete.at
                    << ",\"deltaOperatorFloat\":" << concrete.deltaOperatorFloat
                    << ",\"deltaUserBalance\":" << concrete.deltaUserBalance
                    << ",\"kind\":\"policyAdjustment\""
                    << ",\"note\":" << escapeString(concrete.note)
                    << ",\"seq\":" << concrete.seq << "}";
            }
        },
        entry);
    return oss.str();
}

// Checked parser for the existing writer's JSON-line format. Keys may be in
// any order, but every variant has one exact field set. No missing field is
// supplied from a default and no duplicate/unknown/trailing field is ignored.
struct Parser {
    const std::string& s;
    size_t i{0};
    void skipWs() {
        while (i<s.size() && (s[i]==' ' || s[i]=='\t' || s[i]=='\r' || s[i]=='\n')) ++i;
    }
    bool match(char c) {
        skipWs();
        if(i<s.size() && s[i]==c) {++i;return true;}
        return false;
    }
    void expect(char c) {
        if(!match(c))throw LedgerStoreError("unexpected JSON token");
    }
    void finish() {
        skipWs();
        if(i!=s.size())throw LedgerStoreError("trailing JSON data");
    }
    static int nibble(char c) {
        if(c>='0' && c<='9')return c-'0';
        if(c>='a' && c<='f')return c-'a'+10;
        if(c>='A' && c<='F')return c-'A'+10;
        return -1;
    }
    uint32_t unicodeUnit() {
        if(s.size()-i<4)throw LedgerStoreError("incomplete Unicode escape");
        uint32_t result=0;
        for(unsigned n=0;n<4;++n) {
            const int v=nibble(s[i++]);
            if(v<0)throw LedgerStoreError("invalid Unicode escape");
            result=(result<<4)|static_cast<unsigned>(v);
        }
        return result;
    }
    static void appendUtf8(std::string& out,uint32_t value) {
        if(value<=0x7f)out.push_back(static_cast<char>(value));
        else if(value<=0x7ff) {
            out.push_back(static_cast<char>(0xc0|(value>>6)));
            out.push_back(static_cast<char>(0x80|(value&0x3f)));
        } else if(value<=0xffff) {
            out.push_back(static_cast<char>(0xe0|(value>>12)));
            out.push_back(static_cast<char>(0x80|((value>>6)&0x3f)));
            out.push_back(static_cast<char>(0x80|(value&0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0|(value>>18)));
            out.push_back(static_cast<char>(0x80|((value>>12)&0x3f)));
            out.push_back(static_cast<char>(0x80|((value>>6)&0x3f)));
            out.push_back(static_cast<char>(0x80|(value&0x3f)));
        }
    }
    std::string readString() {
        expect('"');
        std::string out;
        while(i<s.size()) {
            const unsigned char c=static_cast<unsigned char>(s[i++]);
            if(c=='"')return out;
            if(c<0x20)throw LedgerStoreError("unescaped string control byte");
            if(c!='\\') {out.push_back(static_cast<char>(c));continue;}
            if(i==s.size())throw LedgerStoreError("incomplete string escape");
            switch(s[i++]) {
                case '"':out.push_back('"');break;
                case '\\':out.push_back('\\');break;
                case '/':out.push_back('/');break;
                case 'b':out.push_back('\b');break;
                case 'f':out.push_back('\f');break;
                case 'n':out.push_back('\n');break;
                case 'r':out.push_back('\r');break;
                case 't':out.push_back('\t');break;
                case 'u': {
                    uint32_t value=unicodeUnit();
                    if(value>=0xd800 && value<=0xdbff) {
                        if(s.size()-i<2 || s[i]!='\\' || s[i+1]!='u')
                            throw LedgerStoreError("missing low surrogate");
                        i+=2;const uint32_t low=unicodeUnit();
                        if(low<0xdc00 || low>0xdfff)throw LedgerStoreError("invalid low surrogate");
                        value=0x10000+((value-0xd800)<<10)+(low-0xdc00);
                    } else if(value>=0xdc00 && value<=0xdfff)
                        throw LedgerStoreError("unpaired low surrogate");
                    appendUtf8(out,value);break;
                }
                default:throw LedgerStoreError("unknown string escape");
            }
        }
        throw LedgerStoreError("unterminated string");
    }
    template<class T> T integer() {
        skipWs();const size_t start=i;
        if(i<s.size() && s[i]=='-') {
            if constexpr(std::is_unsigned_v<T>)throw LedgerStoreError("negative unsigned integer");
            ++i;
        }
        const size_t digits=i;
        while(i<s.size() && s[i]>='0' && s[i]<='9')++i;
        if(i==digits || (i-digits>1 && s[digits]=='0'))throw LedgerStoreError("invalid integer token");
        T value{};
        const auto result=std::from_chars(s.data()+start,s.data()+i,value);
        if(result.ec!=std::errc{} || result.ptr!=s.data()+i)throw LedgerStoreError("integer out of range");
        return value;
    }
    bool nullValue() {
        skipWs();
        if(s.compare(i,4,"null")==0) {i+=4;return true;}
        return false;
    }
};

OutpointId parseOutpoint(Parser& p) {
    p.expect('{');OutpointId op{};std::set<std::string> fields;
    if(!p.match('}'))while(true) {
        const auto key=p.readString();
        if(!fields.insert(key).second)throw LedgerStoreError("duplicate outpoint field");
        p.expect(':');
        if(key=="txid") {
            if(!hexDecode(p.readString(),op.txid_raw))throw LedgerStoreError("bad txid hex");
        } else if(key=="vout")op.vout=p.integer<uint32_t>();
        else throw LedgerStoreError("unknown outpoint field");
        if(p.match('}'))break;
        p.expect(',');
    }
    if(fields!=std::set<std::string>{"txid","vout"})throw LedgerStoreError("missing outpoint field");
    return op;
}

std::vector<CreditAllocationRef> parseSources(Parser& p) {
    p.expect('[');std::vector<CreditAllocationRef> refs;
    if(p.match(']'))throw LedgerStoreError("empty credit allocation sources");
    uint64_t sum=0;
    while(true) {
        p.expect('{');std::set<std::string> fields;CreditAllocationRef ref;
        do {
            auto key=p.readString();if(!fields.insert(key).second)throw LedgerStoreError("duplicate source field");
            p.expect(':');
            if(key=="amount")ref.amount=p.integer<uint64_t>();
            else if(key=="creditSeq")ref.credit_seq=p.integer<uint64_t>();
            else throw LedgerStoreError("unknown source field");
        } while(p.match(','));
        p.expect('}');
        if(fields!=std::set<std::string>{"amount","creditSeq"} || !ref.amount ||
           (!refs.empty() && refs.back().credit_seq>=ref.credit_seq) || ref.amount>UINT64_MAX-sum ||
           refs.size()>=1000000)throw LedgerStoreError("invalid credit allocation sources");
        sum+=ref.amount;refs.push_back(ref);
        if(p.match(']'))return refs;
        p.expect(',');
    }
}

LedgerEntry parseEntry(const std::string& line) {
    Parser p{line};p.expect('{');std::set<std::string> fields;
    LedgerSeq seq{};LedgerTimestamp at{};AccountId account{};bool account_set=false;
    OutpointId outpoint{};UnaAmount amount{},operator_loss{};BackendId backend{};
    std::string kind,note;int64_t delta_user{},delta_op{};LedgerSeq reversal_seq{},compensation_seq{};
    AllocationRequestId request_id{};std::array<uint8_t,32> body_hash{},block_hash{};
    uint64_t height{};LedgerSeq credit_seq{};std::vector<CreditAllocationRef> sources;
    if(!p.match('}'))while(true) {
        const auto key=p.readString();
        if(!fields.insert(key).second)throw LedgerStoreError("duplicate entry field");
        p.expect(':');
        if(key=="kind")kind=p.readString();
        else if(key=="seq")seq=p.integer<uint64_t>();
        else if(key=="at")at=p.integer<int64_t>();
        else if(key=="account") {
            if(!p.nullValue()) {account.raw=p.readString();account_set=true;}
        } else if(key=="deposit" || key=="request")outpoint=parseOutpoint(p);
        else if(key=="requestId") {if(!hexDecode(p.readString(),request_id))throw LedgerStoreError("bad request id");}
        else if(key=="bodyHash") {if(!hexDecode(p.readString(),body_hash))throw LedgerStoreError("bad body hash");}
        else if(key=="blockHash") {if(!hexDecode(p.readString(),block_hash))throw LedgerStoreError("bad block hash");}
        else if(key=="output")outpoint=parseOutpoint(p);
        else if(key=="creditSeq")credit_seq=p.integer<uint64_t>();
        else if(key=="height")height=p.integer<uint64_t>();
        else if(key=="sources")sources=parseSources(p);
        else if(key=="amount")amount=p.integer<uint64_t>();
        else if(key=="backend")backend.raw=p.readString();
        else if(key=="operatorLoss")operator_loss=p.integer<uint64_t>();
        else if(key=="reversalSeq")reversal_seq=p.integer<uint64_t>();
        else if(key=="compensationSeq")compensation_seq=p.integer<uint64_t>();
        else if(key=="note")note=p.readString();
        else if(key=="deltaUserBalance")delta_user=p.integer<int64_t>();
        else if(key=="deltaOperatorFloat")delta_op=p.integer<int64_t>();
        else throw LedgerStoreError("unknown entry field");
        if(p.match('}'))break;
        p.expect(',');
    }
    p.finish();
    const auto require=[&](std::initializer_list<const char*> additional) {
        std::set<std::string> expected{"kind","seq","at","account"};
        for(const auto* field:additional)expected.insert(field);
        if(fields!=expected)throw LedgerStoreError("incomplete or unexpected entry fields");
        if(kind!="policyAdjustment" && !account_set)throw LedgerStoreError("null entry account");
    };
    if(kind=="withdrawalAllocationReserved") {
        require({"requestId","amount","sources"});uint64_t sum=0;
        for(const auto& ref:sources)sum+=ref.amount; // parseSources checked overflow and order.
        if(!amount || sum!=amount)throw LedgerStoreError("credit allocation amount mismatch");
        return WithdrawalAllocationReserved{seq,at,account,request_id,amount,std::move(sources)};
    }
    if(kind=="withdrawalAllocationDispatchStarted") {require({"requestId"});return WithdrawalAllocationDispatchStarted{seq,at,account,request_id};}
    if(kind=="withdrawalAllocationPaymentBound") {require({"requestId","output","bodyHash","backend"});return WithdrawalAllocationPaymentBound{seq,at,account,request_id,{outpoint,body_hash},backend};}
    if(kind=="withdrawalAllocationIncluded") {require({"requestId","height","blockHash"});return WithdrawalAllocationIncluded{seq,at,account,request_id,{height,block_hash}};}
    if(kind=="withdrawalAllocationDisconnected") {require({"requestId","height","blockHash"});return WithdrawalAllocationDisconnected{seq,at,account,request_id,{height,block_hash}};}
    if(kind=="withdrawalAllocationReleased") {require({"requestId"});return WithdrawalAllocationReleased{seq,at,account,request_id};}
    if(kind=="creditPositionMatured") {require({"deposit","creditSeq"});return CreditPositionMatured{seq,at,account,credit_seq,outpoint};}
    if(kind=="creditPositionReverted") {require({"deposit","creditSeq"});return CreditPositionReverted{seq,at,account,credit_seq,outpoint};}
    if(kind=="creditPositionRestored") {require({"deposit","creditSeq"});return CreditPositionRestored{seq,at,account,credit_seq,outpoint};}
    if(kind=="depositObserved") {require({"deposit","amount"});return DepositObserved{seq,at,account,outpoint,amount};}
    if(kind=="creditOpened") {require({"deposit","amount"});return CreditOpened{seq,at,account,outpoint,amount};}
    if(kind=="creditSettled") {require({"deposit"});return CreditSettled{seq,at,account,outpoint};}
    if(kind=="creditReverted") {require({"deposit"});return CreditReverted{seq,at,account,outpoint};}
    if(kind=="creditReinstated") {require({"deposit","reversalSeq","compensationSeq"});return CreditReinstated{seq,at,account,outpoint,reversal_seq,compensation_seq};}
    if(kind=="withdrawalInitiated") {require({"request","amount","backend"});return WithdrawalInitiated{seq,at,account,outpoint,amount,backend};}
    if(kind=="withdrawalSettled") {require({"request"});return WithdrawalSettled{seq,at,account,outpoint};}
    if(kind=="withdrawalReverted") {require({"request"});return WithdrawalReverted{seq,at,account,outpoint};}
    if(kind=="compensatingDebit") {require({"deposit","amount","operatorLoss"});return CompensatingDebit{seq,at,account,outpoint,amount,operator_loss};}
    if(kind=="policyAdjustment") {
        require({"note","deltaUserBalance","deltaOperatorFloat"});
        return PolicyAdjustment{seq,at,account_set?std::optional<AccountId>{account}:std::nullopt,note,delta_user,delta_op};
    }
    throw LedgerStoreError("unknown entry kind");
}

}  // namespace

FileLedgerStore::FileLedgerStore(std::string path) : path_{std::move(path)} {
    out_.open(path_, std::ios::app | std::ios::binary);
    if (!out_.is_open()) {
        throw LedgerStoreError("failed to open ledger store: " + path_);
    }
}

void FileLedgerStore::append(const LedgerEntry& entry) {
    std::lock_guard<std::mutex> lock(mu_);
    out_ << serializeEntry(entry) << "\n";
    out_.flush();
    if (!out_.good()) {
        throw LedgerStoreError("ledger store write failed: " + path_);
    }
}

std::vector<LedgerEntry> FileLedgerStore::loadAll() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<LedgerEntry> out;
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
        throw LedgerStoreError("ledger store read open failed: " + path_);
    }
    std::string line;
    size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        // The writer terminates every record with a newline. A final prefix
        // without its delimiter is incomplete, even if it resembles JSON.
        if(in.eof())throw LedgerStoreError("unterminated ledger line " + std::to_string(lineno));
        if (line.empty()) {
            continue;
        }
        try {
            auto entry=parseEntry(line);
            if((!out.empty() && entrySeq(entry)<=entrySeq(out.back())))
                throw LedgerStoreError("ledger sequence is not strictly increasing");
            out.push_back(std::move(entry));
        } catch (const LedgerStoreError& e) {
            throw LedgerStoreError("line " + std::to_string(lineno) + ": " + e.what());
        }
    }
    if(in.bad() || !in.eof())throw LedgerStoreError("ledger store read failed: " + path_);
    return out;
}

void FileLedgerStore::flush() {
    std::lock_guard<std::mutex> lock(mu_);
    out_.flush();
}

}  // namespace dinero::vault
