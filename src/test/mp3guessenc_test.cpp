// Include the library's I/O config before any system header, exactly like
// tags.c, so vbrtagdata_t has the same layout here.
extern "C" {
#if defined(_WIN32) && !defined(__WINDOWS__)
#define __WINDOWS__
#endif
#include "mp3g_io_config.h"
#include "mp3guessenc.h"
}

#include <gtest/gtest.h>

#include <cstring>

namespace {

vbrtagdata_t infoTagWithEncoder(const char* encoder, bool verified) {
    vbrtagdata_t tag;
    std::memset(&tag, 0, sizeof(tag));
    tag.infoTag = TAG_LAMECBRTAG;
    std::memcpy(tag.lametag, encoder, std::strlen(encoder));
    tag.lametagVerified = verified ? 1 : 0;
    return tag;
}

TEST(Mp3GuessEncTest, OldLavcInfoTagsStayCaseB) {
    for (const char* encoder : {"Lavc57.10", "Lavc58.13", "Lavc59.37"}) {
        vbrtagdata_t tag = infoTagWithEncoder(encoder, true);
        EXPECT_EQ(EXIT_CODE_CASE_B, check_timing_shift_case(&tag)) << encoder;
    }
}

TEST(Mp3GuessEncTest, FFmpeg6LavcInfoTagsAreTimedLikeLame) {
    for (const char* encoder : {"Lavc60.6.", "Lavc61.3.", "Lavc100.1"}) {
        vbrtagdata_t verified = infoTagWithEncoder(encoder, true);
        EXPECT_EQ(EXIT_CODE_CASE_D, check_timing_shift_case(&verified)) << encoder;
        vbrtagdata_t unverified = infoTagWithEncoder(encoder, false);
        EXPECT_EQ(EXIT_CODE_CASE_C, check_timing_shift_case(&unverified)) << encoder;
    }
}

TEST(Mp3GuessEncTest, LameAndMissingTagsKeepTheirCases) {
    vbrtagdata_t lame = infoTagWithEncoder("LAME3.100", true);
    EXPECT_EQ(EXIT_CODE_CASE_D, check_timing_shift_case(&lame));
    vbrtagdata_t empty = infoTagWithEncoder("", true);
    EXPECT_EQ(EXIT_CODE_CASE_B, check_timing_shift_case(&empty));
}

} // namespace
