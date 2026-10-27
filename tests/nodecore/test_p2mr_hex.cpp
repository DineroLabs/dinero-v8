#include "nodecore/p2mr_hex.h"
#include <iostream>
int main() {
    using dinero::nodecore::DecodeP2mrHex;
    if (DecodeP2mrHex("00aAFf") != std::vector<uint8_t>({0,170,255})) return 1;
    for (const auto* text : {"0g", "1z", "+1", " 1", "-1", "0x", "0", "gg"}) {
        bool refused=false;
        try { (void)DecodeP2mrHex(text); } catch (...) { refused=true; }
        if (!refused) { std::cerr << "accepted malformed hex: " << text << '\n'; return 1; }
    }
    std::cout << "PASS P2MR hex strictly consumes every digit\n";
}
