#pragma once
#include "vault/ledger_store.h"
#include "vault/ledger.h"
#include <fstream>
#include <limits>
#include <chrono>
#include <atomic>

namespace dinero {
namespace {
struct VaultLedgerReadFiles {
    std::filesystem::path path;
    VaultLedgerReadFiles() {
        static std::atomic<uint64_t> next{0};
        path=std::filesystem::temp_directory_path()/
            ("dinero-ledger-read-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
             "-"+std::to_string(next.fetch_add(1))+".jsonl");
        if(std::filesystem::exists(path))throw std::runtime_error("unexpected test path");
    }
    ~VaultLedgerReadFiles() {std::error_code ec;std::filesystem::remove(path,ec);std::filesystem::remove(path.string()+".saved",ec);}
    void write(const std::string& bytes) {
        std::ofstream f(path,std::ios::binary|std::ios::trunc);f.write(bytes.data(),bytes.size());f.close();
        if(!f)throw std::runtime_error("fixture write failed");
    }
    std::string read() const {
        std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("fixture read failed");
        return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
    }
    std::vector<vault::LedgerEntry> load() const {vault::FileLedgerStore store(path.string());return store.loadAll();}
};
std::string LedgerReadRow(uint64_t seq=1) {
    return "{\"account\":\"owner\",\"amount\":100,\"at\":-7,\"deposit\":{\"txid\":\""+
        std::string(64,'1')+"\",\"vout\":2},\"kind\":\"depositObserved\",\"seq\":"+std::to_string(seq)+"}";
}
std::string LedgerReadReplace(std::string value,const std::string& from,const std::string& to) {
    const auto at=value.find(from);if(at==std::string::npos)throw std::runtime_error("fixture replacement missing");
    value.replace(at,from.size(),to);return value;
}
void LedgerReadRefuses(VaultLedgerReadFiles& file,const std::string& bytes) {
    file.write(bytes);
    EXPECT_THROW(file.load(),vault::LedgerStoreError);
    EXPECT_EQ(file.read(),bytes);
}
}
TEST(VaultLedgerRead, ExactExistingVariantsAndEscapedBytes) {
    VaultLedgerReadFiles f;vault::OutpointId op{};op.txid_raw.fill(0xab);op.vout=UINT32_MAX;
    std::string account="quote\" slash\\ newline\n braces{} ";
    for(char c=0;c<32;++c)account.push_back(c);
    account+="\xc3\xa9";
    const vault::AccountId a{account};const auto lo=std::numeric_limits<int64_t>::min(),hi=std::numeric_limits<int64_t>::max();
    std::vector<vault::LedgerEntry> entries{
        vault::DepositObserved{1,lo,a,op,UINT64_MAX},vault::CreditOpened{2,hi,a,op,17},
        vault::CreditSettled{3,-1,a,op},vault::CreditReverted{4,0,a,op},
        vault::WithdrawalInitiated{5,7,a,op,91,vault::BackendId{"backend\n\"{}"}},
        vault::WithdrawalSettled{6,0,a,op},vault::WithdrawalReverted{7,0,a,op},
        vault::CompensatingDebit{8,0,a,op,UINT64_MAX,UINT64_MAX},
        vault::PolicyAdjustment{9,lo,a,account,lo,hi},vault::PolicyAdjustment{10,hi,std::nullopt,account,hi,lo}};
    {
        vault::FileLedgerStore store(f.path.string());for(const auto& e:entries)store.append(e);store.flush();
        EXPECT_EQ(store.loadAll(),entries);
    }
    const auto saved=f.read();EXPECT_EQ(f.load(),entries);EXPECT_EQ(f.read(),saved);
    // Reordered fields, legal whitespace and Unicode escapes remain lossless.
    f.write("{ \"seq\":11, \"kind\":\"policyAdjustment\", \"at\":-2, \"account\":null, "
            "\"deltaUserBalance\":0, \"deltaOperatorFloat\":0, \"note\":\"\\u00e9\\ud83d\\ude00\\b\\f\\/\" }\n");
    const auto loaded=f.load();ASSERT_EQ(loaded.size(),1u);
    EXPECT_EQ(std::get<vault::PolicyAdjustment>(loaded[0]).note,"\xc3\xa9\xf0\x9f\x98\x80\b\f/");
}
TEST(VaultLedgerRead, CompleteVariantFieldsAndTerminalInputRequired) {
    VaultLedgerReadFiles f;const auto row=LedgerReadRow(2);std::vector<std::string> bad;
    bad.push_back(LedgerReadReplace(row,"\"amount\":100,",""));
    bad.push_back(LedgerReadReplace(row,"\"account\":\"owner\"","\"account\":null"));
    bad.push_back(LedgerReadReplace(row,"\"amount\":100","\"amount\":100,\"amount\":100"));
    bad.push_back(LedgerReadReplace(row,"\"kind\":\"depositObserved\"","\"kind\":\"creditSettled\""));
    bad.push_back(LedgerReadReplace(row,"\"deposit\"","\"request\""));
    bad.push_back(LedgerReadReplace(row,"\"vout\":2","\"vout\":2,\"vout\":2"));
    bad.push_back(LedgerReadReplace(row,",\"vout\":2",""));
    bad.push_back(LedgerReadReplace(row,"\"seq\":2","\"seq\":2,\"unknown\":0"));
    bad.push_back(LedgerReadReplace(row,"\"owner\"","\"text with \\\"kind\\\":\\\"creditOpened\\\"\""));
    // The kind-like text in an account is ordinary data, not a variant selector.
    f.write(bad.back()+"\n");EXPECT_NO_THROW(f.load());bad.pop_back();
    bad.push_back(row+" false");bad.push_back(row+row);bad.push_back(row.substr(0,row.size()-1));
    bad.push_back(LedgerReadReplace(row,"\"seq\":2}","\"seq\":2,}"));
    bad.push_back(LedgerReadReplace(row,"depositObserved","unknown"));
    for(size_t i=0;i<bad.size();++i) {SCOPED_TRACE(i);LedgerReadRefuses(f,LedgerReadRow()+"\n"+bad[i]+"\n");}
    f.write(LedgerReadRow()+"\n"+row+"\n");EXPECT_EQ(f.load().size(),2u);
}
TEST(VaultLedgerRead, ExactNumericHexAndStringDecoding) {
    VaultLedgerReadFiles f;const auto row=LedgerReadRow();
    for(const auto& amount:std::vector<std::string>{"-1","+1","01","1.0","1e2","true","\"1\"","18446744073709551616"}) {
        SCOPED_TRACE(amount);LedgerReadRefuses(f,LedgerReadReplace(row,"\"amount\":100","\"amount\":"+amount)+"\n");
    }
    for(const auto& value:std::vector<std::string>{"4294967296","-1","2.0"})
        LedgerReadRefuses(f,LedgerReadReplace(row,"\"vout\":2","\"vout\":"+value)+"\n");
    for(const auto& value:std::vector<std::string>{"9223372036854775808","-9223372036854775809","--7"})
        LedgerReadRefuses(f,LedgerReadReplace(row,"\"at\":-7","\"at\":"+value)+"\n");
    for(const auto& hex:std::vector<std::string>{"0z","+1"," 1","-1","g0"})
        LedgerReadRefuses(f,LedgerReadReplace(row,std::string(64,'1'),hex+std::string(62,'1'))+"\n");
    for(const auto& value:std::vector<std::string>{"\\q","\\uZZZZ","\\ud800","\\udc00","\\ud800\\u0000",std::string(1,'\x01')})
        LedgerReadRefuses(f,LedgerReadReplace(row,"owner",value)+"\n");
    f.write(row+"\n");EXPECT_EQ(f.load().size(),1u);
}
TEST(VaultLedgerRead, UnavailableIncompleteAndUnorderedReadsRefuse) {
    VaultLedgerReadFiles f;
    const auto first=LedgerReadRow();
    LedgerReadRefuses(f,first); // A complete-looking but unterminated writer line.
    LedgerReadRefuses(f,first+"\n"+LedgerReadRow(2));
    LedgerReadRefuses(f,first+"\n"+first+"\n");
    // The actual ledger begins at sequence zero; preserve that valid format.
    vault::Ledger ledger;vault::OutpointId op{};op.txid_raw.fill(1);
    ledger.append(vault::DepositObserved{ledger.nextSeq(),0,vault::AccountId{"actual"},op,10});
    ASSERT_EQ(vault::entrySeq(ledger.entries().front()),0u);
    f.write("");
    {vault::FileLedgerStore store(f.path.string());store.append(ledger.entries().front());}
    EXPECT_EQ(f.load(),ledger.entries());
    EXPECT_EQ(vault::Ledger::replay(f.load()).entries(),ledger.entries());
    LedgerReadRefuses(f,LedgerReadRow(3)+"\n"+LedgerReadRow(2)+"\n");
    f.write(first+"\n");
    {
        vault::FileLedgerStore store(f.path.string());const auto saved=f.path.string()+".saved";
        std::filesystem::rename(f.path,saved);
        EXPECT_THROW(store.loadAll(),vault::LedgerStoreError);
        EXPECT_FALSE(std::filesystem::exists(f.path));
        std::filesystem::rename(saved,f.path);EXPECT_EQ(store.loadAll().size(),1u);
    }
    f.write("\n"+first+"\n\n"+LedgerReadRow(5)+"\n");EXPECT_EQ(f.load().size(),2u);
    f.write("");EXPECT_TRUE(f.load().empty()); // No fresh-owner/completeness assertion.
}
} // namespace dinero
