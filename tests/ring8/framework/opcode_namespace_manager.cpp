#include "opcode_namespace_manager.h"

namespace dinero {
namespace governance {
namespace test {

OpcodeNamespaceManager::OpcodeNamespaceManager()
    : next_namespace_id_(0)
{
    initializeCoreNamespace();
}

void OpcodeNamespaceManager::initializeCoreNamespace() {
    // Initialize CORE namespace with Ring 7 frozen opcodes
    // These opcodes are IMMUTABLE per Ring 8 Phase 8a (BC2)

    OpcodeNamespace core = OpcodeNamespace::CORE;
    namespace_creation_order_[core] = next_namespace_id_++;

    // Ring 7 opcodes (frozen) - directly insert to bypass registerOpcode check
    namespaces_[core][0x51] = "OP_1";
    namespaces_[core][0x52] = "OP_2";
    namespaces_[core][0x53] = "OP_3";
    namespaces_[core][0x54] = "OP_4";
    namespaces_[core][0x55] = "OP_5";
    namespaces_[core][0x56] = "OP_6";
    namespaces_[core][0x57] = "OP_7";
    namespaces_[core][0x58] = "OP_8";
    namespaces_[core][0x59] = "OP_9";
    namespaces_[core][0x5a] = "OP_10";
    namespaces_[core][0x76] = "OP_DUP";
    namespaces_[core][0x87] = "OP_EQUAL";
    namespaces_[core][0x88] = "OP_EQUALVERIFY";
    namespaces_[core][0x93] = "OP_ADD";
    namespaces_[core][0x94] = "OP_SUB";
    namespaces_[core][0xac] = "OP_CHECKSIG";
    namespaces_[core][0xad] = "OP_CHECKSIGVERIFY";

    // Mark these as core opcodes (frozen)
    core_opcodes_ = getNamespaceOpcodes(core);
}

bool OpcodeNamespaceManager::registerOpcode(
    OpcodeNamespace ns,
    uint8_t opcode,
    const std::string& name)
{
    // EG1: Cannot modify CORE namespace after initialization
    if (ns == OpcodeNamespace::CORE) {
        // Only allow during initialization
        if (!namespaces_[ns].empty()) {
            return false;  // CORE namespace already initialized (frozen)
        }
    }

    // Check if opcode already registered in this namespace
    if (isOpcodeRegistered(ns, opcode)) {
        return false;  // Opcode collision in same namespace
    }

    // Register the opcode
    namespaces_[ns][opcode] = name;

    // Track namespace creation if new
    if (namespace_creation_order_.find(ns) == namespace_creation_order_.end()) {
        namespace_creation_order_[ns] = next_namespace_id_++;
    }

    return true;
}

bool OpcodeNamespaceManager::isOpcodeRegistered(OpcodeNamespace ns, uint8_t opcode) const {
    auto ns_it = namespaces_.find(ns);
    if (ns_it == namespaces_.end()) {
        return false;
    }

    return ns_it->second.find(opcode) != ns_it->second.end();
}

std::optional<std::string> OpcodeNamespaceManager::getOpcodeName(
    OpcodeNamespace ns,
    uint8_t opcode) const
{
    auto ns_it = namespaces_.find(ns);
    if (ns_it == namespaces_.end()) {
        return std::nullopt;
    }

    auto op_it = ns_it->second.find(opcode);
    if (op_it == ns_it->second.end()) {
        return std::nullopt;
    }

    return op_it->second;
}

std::set<uint8_t> OpcodeNamespaceManager::getNamespaceOpcodes(OpcodeNamespace ns) const {
    std::set<uint8_t> opcodes;

    auto ns_it = namespaces_.find(ns);
    if (ns_it == namespaces_.end()) {
        return opcodes;
    }

    for (const auto& [opcode, name] : ns_it->second) {
        opcodes.insert(opcode);
    }

    return opcodes;
}

std::vector<IsolationViolation> OpcodeNamespaceManager::checkNamespaceIsolation() const {
    std::vector<IsolationViolation> violations;

    // EG1: Check that CORE namespace hasn't been modified
    auto core_opcodes = getNamespaceOpcodes(OpcodeNamespace::CORE);
    if (core_opcodes != core_opcodes_) {
        IsolationViolation v("EG1", "CORE namespace modified (Ring 7 frozen)");
        v.opcode_namespace = OpcodeNamespace::CORE;
        v.details = "CORE namespace must remain frozen per Ring 8 Phase 8a (BC2)";
        violations.push_back(v);
    }

    // EG1: Check for opcode collisions across namespaces
    auto collisions = checkOpcodeCollisions();
    for (const std::string& collision : collisions) {
        IsolationViolation v("EG1", "Opcode collision across namespaces");
        v.details = collision;
        violations.push_back(v);
    }

    return violations;
}

bool OpcodeNamespaceManager::checkCoreNamespaceFrozen() const {
    // CORE namespace should match frozen Ring 7 opcodes
    auto core_opcodes = getNamespaceOpcodes(OpcodeNamespace::CORE);
    return core_opcodes == core_opcodes_;
}

std::vector<std::string> OpcodeNamespaceManager::checkOpcodeCollisions() const {
    std::vector<std::string> collisions;

    // Check for same opcode byte used in multiple namespaces
    std::map<uint8_t, std::vector<OpcodeNamespace>> opcode_usage;

    for (const auto& [ns, opcodes] : namespaces_) {
        for (const auto& [opcode, name] : opcodes) {
            opcode_usage[opcode].push_back(ns);
        }
    }

    // Report collisions
    for (const auto& [opcode, namespaces] : opcode_usage) {
        if (namespaces.size() > 1) {
            std::string collision = "Opcode 0x" + std::to_string(opcode) +
                                  " used in " + std::to_string(namespaces.size()) +
                                  " namespaces";
            collisions.push_back(collision);
        }
    }

    return collisions;
}

} // namespace test
} // namespace governance
} // namespace dinero
