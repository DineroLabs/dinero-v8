#include "p2p/structural_validator.h"

#include "primitives/block.h"
#include "primitives/transaction.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace dinero;

namespace {

enum StructuralFuzzMode : uint8_t {
    FUZZ_VALIDATE_BLOCK = 0,
    FUZZ_VALIDATE_TX = 1,
    FUZZ_VALIDATE_AND_DESERIALIZE_BLOCK = 2,
    FUZZ_VALIDATE_AND_DESERIALIZE_TX = 3,
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 2) {
        return 0;
    }

    const StructuralFuzzMode mode = static_cast<StructuralFuzzMode>(data[0] % 4);
    std::vector<uint8_t> payload(data + 1, data + size);
    dinero::p2p::StructuralValidator validator;

    switch (mode) {
        case FUZZ_VALIDATE_BLOCK: {
            (void)validator.validateBlock(payload);
            break;
        }
        case FUZZ_VALIDATE_TX: {
            (void)validator.validateTx(payload);
            break;
        }
        case FUZZ_VALIDATE_AND_DESERIALIZE_BLOCK: {
            const auto result = validator.validateBlock(payload);
            if (result.ok) {
                auto block = Block::Deserialize(payload);
                if (block.has_value()) {
                    (void)block->GetHash();
                    (void)block->vtx.size();
                }
            }
            break;
        }
        case FUZZ_VALIDATE_AND_DESERIALIZE_TX: {
            const auto result = validator.validateTx(payload);
            if (result.ok) {
                Transaction tx;
                size_t consumed = 0;
                if (TransactionSerializer::Deserialize(tx, payload, consumed)) {
                    (void)tx.GetTxid();
                    (void)tx.GetWeight();
                    (void)tx.IsCoinbase();
                }
            }
            break;
        }
    }

    return 0;
}
