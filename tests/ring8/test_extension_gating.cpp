/**
 * Ring 8 Phase 8b: Extension Gating & Activation Tests
 *
 * These tests verify that extensions can be safely added without breaking Ring 7.
 *
 * Properties tested:
 * - EG1: Namespace Isolation - New opcodes don't affect Ring 7 scripts
 * - EG2: Version Isolation - Script version 0 never accesses version 1+ features
 * - EG3: Activation Safety - Extensions activate only when explicitly gated
 *
 * CRITICAL: These tests enable safe evolution while maintaining Ring 7 immutability.
 */

#include <gtest/gtest.h>
#include "framework/extension_types.h"
#include "framework/extension_registry.h"
#include "framework/script_version_dispatcher.h"
#include "framework/opcode_namespace_manager.h"
#include <memory>

using namespace dinero::governance::test;
using namespace dinero::execution::test;

class ExtensionGatingTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry = std::make_shared<ExtensionRegistry>();
        dispatcher = std::make_unique<ScriptVersionDispatcher>(registry);
        namespace_mgr = std::make_unique<OpcodeNamespaceManager>();
    }

    std::shared_ptr<ExtensionRegistry> registry;
    std::unique_ptr<ScriptVersionDispatcher> dispatcher;
    std::unique_ptr<OpcodeNamespaceManager> namespace_mgr;
};

// ============================================================================
// EG1: Namespace Isolation
// ============================================================================

TEST_F(ExtensionGatingTest, EG1_CoreNamespaceImmutable) {
    // CORE namespace (Ring 7) must be frozen
    // Cannot register new opcodes in CORE namespace

    bool registered = namespace_mgr->registerOpcode(
        OpcodeNamespace::CORE,
        0xff,  // New opcode
        "OP_NEW_FEATURE"
    );

    EXPECT_FALSE(registered)
        << "EG1 violation: Cannot modify CORE namespace (Ring 7 frozen)";

    // Verify CORE namespace is still frozen
    EXPECT_TRUE(namespace_mgr->checkCoreNamespaceFrozen())
        << "CORE namespace must remain frozen";
}

TEST_F(ExtensionGatingTest, EG1_ExtensionNamespaceIsolated) {
    // Extension namespaces are isolated from CORE

    // Register extension opcode in EXTENSION_1 namespace
    bool registered = namespace_mgr->registerOpcode(
        OpcodeNamespace::EXTENSION_1,
        0xe0,  // Extension opcode
        "OP_EXT_COVENANT"
    );

    EXPECT_TRUE(registered)
        << "Should allow opcodes in extension namespace";

    // Verify CORE namespace unchanged
    EXPECT_TRUE(namespace_mgr->checkCoreNamespaceFrozen())
        << "CORE namespace must remain frozen after extension registration";

    // Verify no collisions
    auto collisions = namespace_mgr->checkOpcodeCollisions();
    EXPECT_TRUE(collisions.empty())
        << "Extension opcodes must not collide with CORE";
}

TEST_F(ExtensionGatingTest, EG1_NoOpcodeCollisions) {
    // Same opcode byte cannot be used in multiple namespaces

    namespace_mgr->registerOpcode(OpcodeNamespace::EXTENSION_1, 0xe0, "OP_EXT_A");

    bool registered = namespace_mgr->registerOpcode(
        OpcodeNamespace::EXTENSION_2,
        0xe0,  // Same opcode byte
        "OP_EXT_B"
    );

    EXPECT_TRUE(registered)
        << "Different namespaces can use same opcode byte (isolated)";

    // But warn about potential confusion
    auto collisions = namespace_mgr->checkOpcodeCollisions();
    EXPECT_FALSE(collisions.empty())
        << "Should detect opcode byte reuse across namespaces";
}

TEST_F(ExtensionGatingTest, EG1_NamespaceIsolationProperty) {
    // Register some extension opcodes
    namespace_mgr->registerOpcode(OpcodeNamespace::EXTENSION_1, 0xe0, "OP_EXT_1");
    namespace_mgr->registerOpcode(OpcodeNamespace::EXTENSION_2, 0xe1, "OP_EXT_2");

    // Check isolation property
    auto violations = namespace_mgr->checkNamespaceIsolation();

    EXPECT_TRUE(violations.empty())
        << "EG1: Namespace isolation must be maintained";
}

TEST_F(ExtensionGatingTest, EG1_CoreOpcodeCount) {
    // Ring 7 (CORE) has fixed number of opcodes
    auto core_opcodes = namespace_mgr->getNamespaceOpcodes(OpcodeNamespace::CORE);

    EXPECT_EQ(core_opcodes.size(), 17)
        << "CORE namespace should have 17 Ring 7 opcodes (frozen)";
}

TEST_F(ExtensionGatingTest, EG1_ExtensionOpcodeIndependent) {
    // Extension opcodes don't affect CORE opcode lookup

    auto core_op1 = namespace_mgr->getOpcodeName(OpcodeNamespace::CORE, 0x51);
    EXPECT_EQ(core_op1.value(), "OP_1");

    // Register extension opcode
    namespace_mgr->registerOpcode(OpcodeNamespace::EXTENSION_1, 0xe0, "OP_EXT");

    // CORE opcode still resolves correctly
    auto core_op1_after = namespace_mgr->getOpcodeName(OpcodeNamespace::CORE, 0x51);
    EXPECT_EQ(core_op1_after.value(), "OP_1")
        << "Extension registration must not affect CORE namespace";
}

// ============================================================================
// EG2: Version Isolation
// ============================================================================

TEST_F(ExtensionGatingTest, EG2_Version0UsesCoreOnly) {
    // VERSION_0 (Ring 7) scripts can only use CORE opcodes

    std::vector<uint8_t> script = {0x51, 0x52, 0x93};  // OP_1 OP_2 OP_ADD
    WitnessStack witness;

    auto violations = dispatcher->checkVersionIsolation(ScriptVersion::VERSION_0, script);

    EXPECT_TRUE(violations.empty())
        << "VERSION_0 scripts using CORE opcodes should pass EG2";
}

TEST_F(ExtensionGatingTest, EG2_Version0CannotUseExtensionOpcodes) {
    // VERSION_0 scripts cannot use extension opcodes

    std::vector<uint8_t> script = {0xe0};  // OP_EXT (hypothetical)
    WitnessStack witness;

    auto violations = dispatcher->checkVersionIsolation(ScriptVersion::VERSION_0, script);

    EXPECT_FALSE(violations.empty())
        << "EG2 violation: VERSION_0 cannot use extension opcodes";
    EXPECT_EQ(violations[0].property_name, "EG2");
}

TEST_F(ExtensionGatingTest, EG2_Version1NotActivated) {
    // VERSION_1 is not activated (no extensions yet)

    bool active = dispatcher->isVersionActive(ScriptVersion::VERSION_1, 1000);

    EXPECT_FALSE(active)
        << "VERSION_1 should not be activated (no extensions in Phase 8b)";
}

TEST_F(ExtensionGatingTest, EG2_Version0AlwaysActive) {
    // VERSION_0 (Ring 7) is always active

    bool active_at_0 = dispatcher->isVersionActive(ScriptVersion::VERSION_0, 0);
    bool active_at_1000 = dispatcher->isVersionActive(ScriptVersion::VERSION_0, 1000);
    bool active_at_1M = dispatcher->isVersionActive(ScriptVersion::VERSION_0, 1000000);

    EXPECT_TRUE(active_at_0 && active_at_1000 && active_at_1M)
        << "VERSION_0 (Ring 7) must be active at all heights";
}

TEST_F(ExtensionGatingTest, EG2_VersionIsolationExecution) {
    // Executing VERSION_0 script should work
    std::vector<uint8_t> script = {0x51, 0x52, 0x93};  // OP_1 OP_2 OP_ADD
    WitnessStack witness;

    EXPECT_NO_THROW({
        auto trace = dispatcher->executeScript(
            ScriptVersion::VERSION_0,
            script,
            witness,
            1000,
            "eg2_version0"
        );
        EXPECT_TRUE(trace.success);
    }) << "VERSION_0 execution should succeed";
}

TEST_F(ExtensionGatingTest, EG2_ExtensionOpcodeRejectedInVersion0) {
    // Extension opcodes should be rejected in VERSION_0

    std::vector<uint8_t> script = {0xe0};  // OP_EXT (not in Ring 7)
    WitnessStack witness;

    EXPECT_THROW({
        dispatcher->executeScript(
            ScriptVersion::VERSION_0,
            script,
            witness,
            1000,
            "eg2_extension_rejected"
        );
    }, std::runtime_error)
        << "EG2: VERSION_0 must reject extension opcodes";
}

// ============================================================================
// EG3: Activation Safety
// ============================================================================

TEST_F(ExtensionGatingTest, EG3_ExtensionRequiresGating) {
    // Extensions must be version-gated or namespace-gated

    Extension ext("test_ext", "Test extension");
    // No gating (neither version nor namespace)

    auto violations = registry->validateProposal(ext);

    EXPECT_FALSE(violations.empty())
        << "EG3 violation: Extension must have explicit gating";
    EXPECT_NE(violations[0].find("EG3"), std::string::npos);
}

TEST_F(ExtensionGatingTest, EG3_VersionGatingValid) {
    // Extension with version gating is valid

    Extension ext("test_ext", "Test extension");
    ext.target_version = ScriptVersion::VERSION_1;

    auto violations = registry->validateProposal(ext);

    EXPECT_TRUE(violations.empty())
        << "Version-gated extension should pass validation";
}

TEST_F(ExtensionGatingTest, EG3_NamespaceGatingValid) {
    // Extension with namespace gating is valid

    Extension ext("test_ext", "Test extension");
    ext.target_namespace = OpcodeNamespace::EXTENSION_1;

    auto violations = registry->validateProposal(ext);

    EXPECT_TRUE(violations.empty())
        << "Namespace-gated extension should pass validation";
}

TEST_F(ExtensionGatingTest, EG3_CannotModifyVersion0) {
    // Cannot gate extension to VERSION_0 (Ring 7 frozen)

    Extension ext("test_ext", "Test extension");
    ext.target_version = ScriptVersion::VERSION_0;

    auto violations = registry->validateProposal(ext);

    EXPECT_FALSE(violations.empty())
        << "EG2 violation: Cannot modify VERSION_0 (Ring 7 frozen)";
}

TEST_F(ExtensionGatingTest, EG3_CannotModifyCoreNamespace) {
    // Cannot gate extension to CORE namespace (Ring 7 frozen)

    Extension ext("test_ext", "Test extension");
    ext.target_namespace = OpcodeNamespace::CORE;

    auto violations = registry->validateProposal(ext);

    EXPECT_FALSE(violations.empty())
        << "EG1 violation: Cannot modify CORE namespace (Ring 7 frozen)";
}

TEST_F(ExtensionGatingTest, EG3_NoImplicitActivation) {
    // All extensions must have explicit gating

    Extension ext1("ext1", "Extension 1");
    ext1.target_version = ScriptVersion::VERSION_1;
    registry->registerExtension(ext1);

    Extension ext2("ext2", "Extension 2");
    ext2.target_namespace = OpcodeNamespace::EXTENSION_1;
    registry->registerExtension(ext2);

    EXPECT_TRUE(registry->checkNoImplicitActivation())
        << "EG3: All extensions must have explicit gating";
}

TEST_F(ExtensionGatingTest, EG3_ExtensionRegistration) {
    // Valid extension registration

    Extension ext("covenant_v2", "Covenant extensions v2");
    ext.target_version = ScriptVersion::VERSION_1;
    ext.target_namespace = OpcodeNamespace::EXTENSION_1;

    bool registered = registry->registerExtension(ext);

    EXPECT_TRUE(registered)
        << "Valid extension should register successfully";

    auto status = registry->getStatus("covenant_v2");
    EXPECT_TRUE(status.has_value());
    EXPECT_EQ(status.value(), ExtensionStatus::PROPOSED);
}

TEST_F(ExtensionGatingTest, EG3_ExtensionActivationRequiresDependencies) {
    // Extension with unsatisfied dependencies cannot activate

    Extension base("base_ext", "Base extension");
    base.target_version = ScriptVersion::VERSION_1;
    registry->registerExtension(base);

    Extension dependent("dependent_ext", "Dependent extension");
    dependent.target_version = ScriptVersion::VERSION_2;
    dependent.depends_on = {"base_ext"};
    registry->registerExtension(dependent);

    // Try to activate dependent without activating base
    bool activated = registry->activateExtension("dependent_ext", 1000);

    EXPECT_FALSE(activated)
        << "EG3: Cannot activate extension with unsatisfied dependencies";
}

TEST_F(ExtensionGatingTest, EG3_ExtensionActivationWithDependencies) {
    // Extension with satisfied dependencies can activate

    Extension base("base_ext", "Base extension");
    base.target_version = ScriptVersion::VERSION_1;
    registry->registerExtension(base);
    registry->activateExtension("base_ext", 1000);

    Extension dependent("dependent_ext", "Dependent extension");
    dependent.target_version = ScriptVersion::VERSION_2;
    dependent.depends_on = {"base_ext"};
    registry->registerExtension(dependent);

    // Now activate dependent (dependencies satisfied)
    bool activated = registry->activateExtension("dependent_ext", 2000);

    EXPECT_TRUE(activated)
        << "Extension with satisfied dependencies should activate";
}

TEST_F(ExtensionGatingTest, EG3_ConflictingExtensionsCannotActivate) {
    // Conflicting extensions cannot both be activated

    Extension ext1("ext1", "Extension 1");
    ext1.target_version = ScriptVersion::VERSION_1;
    ext1.conflicts_with = {"ext2"};
    registry->registerExtension(ext1);
    registry->activateExtension("ext1", 1000);

    Extension ext2("ext2", "Extension 2");
    ext2.target_version = ScriptVersion::VERSION_2;
    registry->registerExtension(ext2);

    // Try to activate conflicting extension
    bool activated = registry->activateExtension("ext2", 2000);

    EXPECT_FALSE(activated)
        << "EG3: Conflicting extensions cannot both activate";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
