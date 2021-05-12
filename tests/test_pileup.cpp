#include "pileup.hpp"
#include "util/pileup_reader.hpp"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace {

using namespace ::testing;

/**
 * Tests that reading BAM files where all positions are identical returns an empty result.
 */
TEST(pileup, read_identical) {
    uint32_t chromosome_id = 0;
    uint32_t max_coverage = 10;
    std::vector<PosData> data
            = pileup_bams({ "data/test1.bam", "data/test1.bam", "data/test1.bam" },
                          "data/test_pileup", true, chromosome_id, max_coverage, 1, 1);
    ASSERT_EQ(0, data.size());

    auto [data2, cell_ids, max_len] = read_pileup("data/test_pileup.bin", { 0, 1 });
    ASSERT_EQ(0, data2.size());
}

/**
 * Checks that the content of #data matches what's in test1.sam and test2.sam
 */
void check_content(const std::vector<PosData> &data) {
    ASSERT_EQ(9, data.size());
    for (uint32_t i = 0; i < 4; ++i) {
        ASSERT_EQ(2, data[i].size());
        ASSERT_EQ(0, data[i].read_ids[0]);
        ASSERT_EQ(1, data[i].read_ids[1]);
        ASSERT_EQ(0, data[i].base(0));
        ASSERT_EQ(2, data[i].base(1));
        ASSERT_EQ(0, data[i].cell_id(0));
        ASSERT_EQ(1, data[i].cell_id(1));
    }
    for (uint32_t i = 4; i < 9; ++i) {
        ASSERT_EQ(2, data[i].size());
        ASSERT_EQ(0, data[i].read_ids[0]);
        ASSERT_EQ(1, data[i].read_ids[1]);
        ASSERT_EQ(1, data[i].base(0));
        ASSERT_EQ(3, data[i].base(1));
        ASSERT_EQ(0, data[i].cell_id(0));
        ASSERT_EQ(1, data[i].cell_id(1));
    }
}

/**
 * Tests that the data returned by #pileup_bams matches what's in test1.bam and test2.bam
 */
TEST(pileup, read) {
    uint32_t chromosome_id = 0;
    uint32_t max_coverage = 10;
    std::vector<PosData> data
            = pileup_bams({ "data/test1.bam", "data/test2.bam" }, "data/test_pileup_1", true,
                          chromosome_id, max_coverage, 1, 1);
    check_content(data);
    std::filesystem::remove_all("data/test_pileup_1*");
}

/**
 * Tests that the pileup file generated by #pileup_bams matches what's in test1.bam and test2.bam
 */
TEST(pileup, read_file) {
    uint32_t chromosome_id = 0;
    uint32_t max_coverage = 10;
    pileup_bams({ "data/test1.bam", "data/test2.bam" }, "data/test_pileup_2", true, chromosome_id,
                max_coverage, 1, 1);
    auto [data, cell_ids, max_len] = read_pileup("data/test_pileup_2.bin", { 0, 1 });
    // first pos is 2 because pos 1 is eliminated, last post is 425 -> 425-2 = 423
    ASSERT_EQ(423, max_len);
    ASSERT_THAT(cell_ids, UnorderedElementsAre(0,1));
    check_content(data);
    std::filesystem::remove_all("data/test_pileup_2*");
}

/**
 * Tests that soft clippings are correctly handled by the pileup reader.
 */
TEST(pileup, soft_clipping) {
    uint32_t chromosome_id = 0;
    uint32_t max_coverage = 10;
    pileup_bams({ "data/soft_clipping.bam", "data/test2.bam" }, "data/test_pileup_3", true, chromosome_id,
                max_coverage, 1, 1);
    auto [data, cell_ids, max_len] = read_pileup("data/test_pileup_3.bin", { 0, 1 });
    // first pos is 2 because pos 1 is eliminated, last post is 425 -> 425-2 = 423
    ASSERT_EQ(423, max_len);
    ASSERT_THAT(cell_ids, UnorderedElementsAre(0,1));
    check_content(data);
    std::filesystem::remove_all("data/test_pileup_3*");
}

/**
 * Tests that hard clippings are correctly handled by the pileup reader.
 */
TEST(pileup, hard_clipping) {
    uint32_t chromosome_id = 0;
    uint32_t max_coverage = 10;
    pileup_bams({ "data/hard_clipping.bam", "data/test2.bam" }, "data/test_pileup_4", true, chromosome_id,
                max_coverage, 1, 1);
    auto [data, cell_ids, max_len] = read_pileup("data/test_pileup_4.bin", { 0, 1 });
    // first pos is 2 because pos 1 is eliminated, last post is 425 -> 425-2 = 423
    ASSERT_EQ(423, max_len);
    ASSERT_THAT(cell_ids, UnorderedElementsAre(0,1));
    check_content(data);
    std::filesystem::remove_all("data/test_pileup_4*");
}


} // namespace
