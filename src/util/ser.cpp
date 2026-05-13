#include "util/ser.h"

namespace dinero {
namespace ser {

void writeCompactSize(uint64_t v, std::vector<uint8_t>& out) {
    if (v < 253) {
        out.push_back(uint8_t(v));
        return;
    }
    if (v <= 0xFFFF) {
        out.push_back(0xFD);
        writeLE<uint16_t>(uint16_t(v), out);
        return;
    }
    if (v <= 0xFFFFFFFF) {
        out.push_back(0xFE);
        writeLE<uint32_t>(uint32_t(v), out);
        return;
    }
    out.push_back(0xFF);
    writeLE<uint64_t>(v, out);
}

bool readCompactSize(const uint8_t* data, size_t len, VarIntDecode& out) {
    if (len == 0) return false;
    
    const uint8_t tag = data[0];
    if (tag < 253) {
        out = { tag, 1, 1 };
        return true;
    }

    if (tag == 0xFD) {
        uint16_t v;
        if (!readLE<uint16_t>(data+1, len-1, v)) return false;
        if (v < 253) return false;  // non-canonical
        out = { v, 3, 3 };
        return true;
    }
    
    if (tag == 0xFE) {
        uint32_t v;
        if (!readLE<uint32_t>(data+1, len-1, v)) return false;
        if (v <= 0xFFFF) return false;  // non-canonical
        out = { v, 5, 5 };
        return true;
    }
    
    // tag == 0xFF
    uint64_t v;
    if (!readLE<uint64_t>(data+1, len-1, v)) return false;
    if (v <= 0xFFFFFFFFull) return false;  // non-canonical
    out = { v, 9, 9 };
    return true;
}

} // namespace ser
} // namespace dinero
