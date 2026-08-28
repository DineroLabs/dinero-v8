#include "rpc/rpc_registry.h"

#include <gtest/gtest.h>

namespace {

din::Json NumberResult(int value) {
    din::Json result = din::obj();
    result["value"] = value;
    return result;
}

TEST(RpcRegistryRestartCycle, RestoresStaticBaselineAndReplacesRuntimeContext) {
    RpcRegistry registry;
    EXPECT_TRUE(registry.registerHandler(
        "static.method",
        [](const ExecutionContext&, const din::Json&) { return NumberResult(7); },
        "static"));

    registry.beginRuntimeRegistrationCycle();

    int first_context = 11;
    EXPECT_TRUE(registry.registerHandler(
        "runtime.method",
        [&first_context](const ExecutionContext&, const din::Json&) {
            return NumberResult(first_context);
        },
        "runtime"));
    ASSERT_NE(registry.lookup("runtime.method"), nullptr);
    EXPECT_EQ((*registry.lookup("runtime.method"))(ExecutionContext{}, din::obj())["value"].asInt(), 11);

    registry.beginRuntimeRegistrationCycle();
    EXPECT_TRUE(registry.has("static.method"));
    EXPECT_FALSE(registry.has("runtime.method"));

    int second_context = 29;
    EXPECT_TRUE(registry.registerHandler(
        "runtime.method",
        [&second_context](const ExecutionContext&, const din::Json&) {
            return NumberResult(second_context);
        },
        "runtime"));
    ASSERT_NE(registry.lookup("runtime.method"), nullptr);
    EXPECT_EQ((*registry.lookup("runtime.method"))(ExecutionContext{}, din::obj())["value"].asInt(), 29);
}

}  // namespace
