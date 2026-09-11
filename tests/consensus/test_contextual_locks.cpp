#include "consensus/contextual_locks.h"
#include <iostream>
using namespace dinero;
using namespace dinero::consensus;
int main() {
    int failures = 0;
    auto expect = [&](bool ok, const char* name) { if (!ok) { std::cerr << name << "\n"; ++failures; } };
    Transaction tx; tx.vin.resize(1); tx.vin[0].sequence = 3;
    std::string error;
    auto check = [&](uint32_t h, std::optional<uint32_t> origin) {
        return CheckContextualLocks(tx, h, 111000, {origin},
            [](uint32_t height)->std::optional<uint64_t> { return uint64_t(height)*600; }, error);
    };
    expect(check(110999,110999), "historical acceptance preserved");
    expect(!check(111000,110998), "relative immature at activation");
    expect(check(111001,110998), "relative mature at exact boundary");
    expect(!check(111001,111001), "unconfirmed parent delay");
    expect(!check(111001,std::nullopt), "missing input height rejected");
    tx.vin[0].sequence = 0;
    expect(check(111001,111001), "zero delay allows same block");
    tx.vin[0].sequence = 0xfffffffe;
    tx.lockTime = 111000;
    expect(!check(111000,0), "absolute equality not final");
    expect(check(111001,0), "absolute strictly past lock final");
    tx.vin[0].sequence = UINT32_MAX;
    expect(check(111000,0), "all final disables absolute lock");
    tx.lockTime = 0; tx.vin[0].sequence = (1U<<22)|2;
    expect(!check(111000,111000), "relative time immature");
    expect(check(111000,110998), "relative time maturity");
    tx.version = 1;
    expect(check(111000,std::nullopt), "version one does not enforce sequence locks");
    tx.version = 2; tx.vin[0].sequence = 65535;
    expect(!check(UINT32_MAX,UINT32_MAX-1), "height addition cannot wrap");
    expect(CheckContextualLocks(tx,111000,111000,{0},{},error), "height lock requires no MTP");
    return failures ? 1 : 0;
}
