/**
 * Focused regression smoke test for BlockHeader reserved[12] initialization in
 * mining objects. This stays intentionally tiny so we can keep it in the build
 * graph without pulling in the full daemon stack.
 */

#include <gtest/gtest.h>

#include "mining/block_assembler.h"
#include "mining/block_template.h"

using namespace dinero;

TEST(MiningHeaderReservedSmoke, MiningJobStartsWithZeroReservedBytes) {
    MiningJob job;
    EXPECT_TRUE(job.header.IsReservedValid());
}

TEST(MiningHeaderReservedSmoke, BlockTemplateStartsWithZeroReservedBytes) {
    mining::BlockTemplate tmpl;
    EXPECT_TRUE(tmpl.block.header.IsReservedValid());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
