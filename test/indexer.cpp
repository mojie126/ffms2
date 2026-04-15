#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <random>
#include <vector>

#include <ffms.h>
#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/packet.h>
}

#include "../src/core/track.h"

#include "data/test.mp4.cpp"
#include "data/vp9_audfirst.webm.cpp"
#include "data/qrvideo_24fps_1elist_1ctts.mov.cpp"
#include "data/qrvideo_24fps_1elist_ends_last_bframe.mov.cpp"
#include "data/qrvideo_24fps_1elist_noctts.mov.cpp"
#include "data/qrvideo_24fps_2elist_elist1_dur_zero.mov.cpp"
#include "data/qrvideo_24fps_2elist_elist1_ends_bframe.mov.cpp"
#include "data/qrvideo_24fps_2s_3elist.mov.cpp"
#include "data/qrvideo_24fps_3elist_1ctts.mov.cpp"
#include "data/qrvideo_24fps_elist_starts_ctts_2ndsample.mov.cpp"
#include "data/qrvideo_stream_shorter_than_movie.mov.cpp"
#include "tests.h"


namespace {

const TestDataMap TestFiles[] = {
    TEST_ENTRY("test.mp4", testmp4data),
    TEST_ENTRY("vp9_audfirst.webm", vp9audfirst),
    TEST_ENTRY("qrvideo_24fps_1elist_1ctts.mov", qrvideo_24fps_1elist_1ctts),
    TEST_ENTRY("qrvideo_24fps_1elist_ends_last_bframe.mov", qrvideo_24fps_1elist_ends_last_bframe),
    TEST_ENTRY("qrvideo_24fps_1elist_noctts.mov", qrvideo_24fps_1elist_noctts),
    TEST_ENTRY("qrvideo_24fps_2elist_elist1_dur_zero.mov", qrvideo_24fps_2elist_elist1_dur_zero),
    TEST_ENTRY("qrvideo_24fps_2elist_elist1_ends_bframe.mov", qrvideo_24fps_2elist_elist1_ends_bframe),
    TEST_ENTRY("qrvideo_24fps_2s_3elist.mov", qrvideo_24fps_2s_3elist),
    TEST_ENTRY("qrvideo_24fps_3elist_1ctts.mov", qrvideo_24fps_3elist_1ctts),
    TEST_ENTRY("qrvideo_24fps_elist_starts_ctts_2ndsample.mov", qrvideo_24fps_elist_starts_ctts_2ndsample),
    TEST_ENTRY("qrvideo_stream_shorter_than_movie.mov", qrvideo_stream_shorter_than_movie),
};

void WriteLE16(std::ofstream &out, uint16_t value) {
    const std::array<char, 2> bytes = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
    };
    out.write(bytes.data(), bytes.size());
}

void WriteLE32(std::ofstream &out, uint32_t value) {
    const std::array<char, 4> bytes = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 24) & 0xFF),
    };
    out.write(bytes.data(), bytes.size());
}

std::filesystem::path CreateTempWaveFile() {
    const auto unique_stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("ffms2-indexer-api-" + std::to_string(unique_stamp) + ".wav");

    std::ofstream out(path, std::ios::binary);
    EXPECT_TRUE(out.is_open());

    const std::array<uint8_t, 16> samples = {
        128, 128, 128, 128, 128, 128, 128, 128,
        128, 128, 128, 128, 128, 128, 128, 128,
    };
    const uint16_t channels = 1;
    const uint32_t sample_rate = 8000;
    const uint16_t bits_per_sample = 8;
    const uint16_t block_align = channels * bits_per_sample / 8;
    const uint32_t byte_rate = sample_rate * block_align;
    const uint32_t data_size = static_cast<uint32_t>(samples.size());

    out.write("RIFF", 4);
    WriteLE32(out, 36 + data_size);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    WriteLE32(out, 16);
    WriteLE16(out, 1);
    WriteLE16(out, channels);
    WriteLE32(out, sample_rate);
    WriteLE32(out, byte_rate);
    WriteLE16(out, block_align);
    WriteLE16(out, bits_per_sample);
    out.write("data", 4);
    WriteLE32(out, data_size);
    out.write(reinterpret_cast<const char *>(samples.data()), samples.size());
    out.close();

    EXPECT_TRUE(std::filesystem::exists(path));
    return path;
}

struct TempFileGuard {
    explicit TempFileGuard(std::filesystem::path path) : path(std::move(path)) {}
    ~TempFileGuard() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    std::filesystem::path path;
};

class IndexerTest : public ::testing::TestWithParam<TestDataMap> {
protected:
    virtual void SetUp();
    virtual void TearDown();
    bool DoIndexing(std::string);

    FFMS_Indexer* indexer;
    FFMS_Index* index;
    int video_track_idx;
    FFMS_VideoSource* video_source;
    const FFMS_VideoProperties* VP;

    FFMS_ErrorInfo E;
    char ErrorMsg[1024];

    std::string SamplesDir;
};

void IndexerTest::SetUp() {
    indexer = nullptr;
    index = nullptr;
    video_track_idx = -1;
    video_source = nullptr;
    VP = nullptr;
    E.Buffer = ErrorMsg;
    E.BufferSize = sizeof(ErrorMsg);
    const char *SamplesDirEnv = std::getenv("FFMS2_SAMPLES_DIR");
    if (SamplesDirEnv && SamplesDirEnv[0] != '\0')
        SamplesDir = SamplesDirEnv;
    else
        SamplesDir = STRINGIFY(SAMPLES_DIR);

    FFMS_Init(0,0);
}

void IndexerTest::TearDown() {
    FFMS_DestroyIndex(index);
    FFMS_DestroyVideoSource(video_source);
}

bool IndexerTest::DoIndexing(std::string file_name) {
    indexer = FFMS_CreateIndexer(file_name.c_str(), &E);
    NULL_CHECK(indexer);
    FFMS_TrackTypeIndexSettings(indexer, FFMS_TYPE_VIDEO, 1, 0);

    index = FFMS_DoIndexing2(indexer, 0, &E);
    NULL_CHECK(index);

    video_track_idx = FFMS_GetFirstTrackOfType(index, FFMS_TYPE_VIDEO, &E);
    EXPECT_GE(0, video_track_idx);

    video_source = FFMS_CreateVideoSource(file_name.c_str(), video_track_idx, index, 1, FFMS_SEEK_NORMAL, &E, "none", 0);
    NULL_CHECK(video_source);

    VP = FFMS_GetVideoProperties(video_source); // Can't fail

    return true;
}

TEST(FFMSTrackTest, FindPacket_PrefersFullMatch) {
    FFMS_Track track(1, 1000, FFMS_TYPE_VIDEO, false, false);
    track.AddVideoFrame(100, 100, 0, true, 0, 10, false, false);
    track.AddVideoFrame(100, 100, 0, false, 0, 20, true, false);

    AVPacket packet{};
    packet.pts = 100;
    packet.dts = 100;
    packet.pos = 20;
    packet.flags = AV_PKT_FLAG_DISCARD;

    EXPECT_EQ(1, track.FindPacket(packet));
}

TEST(FFMSTrackTest, FindPacket_FallsBackWithoutKey) {
    FFMS_Track track(1, 1000, FFMS_TYPE_VIDEO, false, false);
    track.AddVideoFrame(100, 100, 0, true, 0, 10, false, false);

    AVPacket packet{};
    packet.pts = 100;
    packet.dts = 100;
    packet.pos = 10;
    packet.flags = 0;

    EXPECT_EQ(0, track.FindPacket(packet));
}

TEST(FFMSTrackTest, FindPacket_UsesDTSWhenConfigured) {
    FFMS_Track track(1, 1000, FFMS_TYPE_VIDEO, false, true);
    track.AddVideoFrame(200, 200, 0, false, 0, 33, false, false);

    AVPacket packet{};
    packet.pts = 999;
    packet.dts = 200;
    packet.pos = 33;
    packet.flags = 0;

    EXPECT_EQ(0, track.FindPacket(packet));
}

TEST(FFMSTrackTest, FindPacket_AmbiguousReturnsMinusOne) {
    FFMS_Track track(1, 1000, FFMS_TYPE_VIDEO, false, false);
    track.AddVideoFrame(100, 100, 0, false, 0, 10, false, false);
    track.AddVideoFrame(100, 100, 0, false, 0, 10, false, false);

    AVPacket packet{};
    packet.pts = 100;
    packet.dts = 100;
    packet.pos = 10;
    packet.flags = 0;

    EXPECT_EQ(-1, track.FindPacket(packet));
}

TEST(FFMSIndexerApiTest, InvalidTrackAndNullMetadataKeyReturnNullishResults) {
    TempFileGuard temp_file(CreateTempWaveFile());

    FFMS_ErrorInfo error_info{};
    char error_msg[1024] = {};
    error_info.Buffer = error_msg;
    error_info.BufferSize = sizeof(error_msg);

    std::unique_ptr<FFMS_Indexer, decltype(&FFMS_CancelIndexing)> indexer(
        FFMS_CreateIndexer(temp_file.path.string().c_str(), &error_info),
        FFMS_CancelIndexing
    );
    ASSERT_NE(nullptr, indexer);

    EXPECT_EQ(FFMS_TYPE_AUDIO, FFMS_GetTrackTypeI(indexer.get(), 0));
    EXPECT_NE(nullptr, FFMS_GetCodecNameI(indexer.get(), 0));

    EXPECT_EQ(FFMS_TYPE_UNKNOWN, FFMS_GetTrackTypeI(indexer.get(), -1));
    EXPECT_EQ(FFMS_TYPE_UNKNOWN, FFMS_GetTrackTypeI(indexer.get(), 99));
    EXPECT_EQ(nullptr, FFMS_GetCodecNameI(indexer.get(), -1));
    EXPECT_EQ(nullptr, FFMS_GetCodecNameI(indexer.get(), 99));
    EXPECT_EQ(nullptr, FFMS_GetTrackMetadataI(indexer.get(), -1, "title"));
    EXPECT_EQ(nullptr, FFMS_GetTrackMetadataI(indexer.get(), 99, "title"));
    EXPECT_EQ(nullptr, FFMS_GetTrackMetadataI(indexer.get(), 0, nullptr));
    EXPECT_EQ(nullptr, FFMS_GetTrackMetadataI(indexer.get(), 0, ""));
}

TEST_P(IndexerTest, ValidateFrameCount) {
    TestDataMap P = GetParam();
    std::string FilePath = SamplesDir + "/" + P.Filename;
    if (!std::filesystem::exists(FilePath))
        GTEST_SKIP() << "Missing sample file: " << FilePath << ". Set FFMS2_SAMPLES_DIR to run this test.";

    ASSERT_TRUE(DoIndexing(FilePath));

    ASSERT_EQ(P.TestDataLen, VP->NumFrames);
}

TEST_P(IndexerTest, ReverseAccessingFrame) {
    TestDataMap P = GetParam();
    std::string FilePath = SamplesDir + "/" + P.Filename;
    if (!std::filesystem::exists(FilePath))
        GTEST_SKIP() << "Missing sample file: " << FilePath << ". Set FFMS2_SAMPLES_DIR to run this test.";

    ASSERT_TRUE(DoIndexing(FilePath));
    for (int i = VP->NumFrames - 1; i > 0; i--) {
        std::stringstream ss;
        ss << "Testing Frame: " << i;
        SCOPED_TRACE(ss.str());

        FFMS_Track *track = FFMS_GetTrackFromIndex(index, video_track_idx);
        const FFMS_FrameInfo *info = FFMS_GetFrameInfo(track, i);

        const FFMS_Frame* frame = FFMS_GetFrame(video_source, i, &E);
        ASSERT_NE(nullptr, frame);
        ASSERT_TRUE(CheckFrame(frame, info, &P.TestData[i]));
    }
}

TEST_P(IndexerTest, RandomAccessingFrame) {
    TestDataMap P = GetParam();
    std::string FilePath = SamplesDir + "/" + P.Filename;
    if (!std::filesystem::exists(FilePath))
        GTEST_SKIP() << "Missing sample file: " << FilePath << ". Set FFMS2_SAMPLES_DIR to run this test.";

    ASSERT_TRUE(DoIndexing(FilePath));

    std::vector<int> FrameNums;
    for (int i = 0; i < VP->NumFrames; i++)
        FrameNums.push_back(i);

    std::random_device Device;
    std::mt19937 Gen(Device());

    std::shuffle(FrameNums.begin(), FrameNums.end(), Gen);

    for (int i = 0; i < VP->NumFrames; i++) {
        int num = FrameNums[i];

        std::stringstream ss;
        ss << "Testing Frame: " << num;
        SCOPED_TRACE(ss.str());

        FFMS_Track *track = FFMS_GetTrackFromIndex(index, video_track_idx);
        const FFMS_FrameInfo *info = FFMS_GetFrameInfo(track, num);

        const FFMS_Frame* frame = FFMS_GetFrame(video_source, num, &E);
        ASSERT_NE(nullptr, frame);
        ASSERT_TRUE(CheckFrame(frame, info, &P.TestData[num]));
    }
}

TEST_P(IndexerTest, HardwareDecoderSeekConsistency) {
    TestDataMap P = GetParam();
    std::string FilePath = SamplesDir + "/" + P.Filename;
    if (!std::filesystem::exists(FilePath))
        GTEST_SKIP() << "Missing sample file: " << FilePath << ". Set FFMS2_SAMPLES_DIR to run this test.";

    ASSERT_TRUE(DoIndexing(FilePath));
    FFMS_Track *track = FFMS_GetTrackFromIndex(index, video_track_idx);
    ASSERT_NE(nullptr, track);

    std::vector<int> probe_frames = {0};
    if (VP->NumFrames > 2)
        probe_frames.push_back(VP->NumFrames / 2);
    if (VP->NumFrames > 1)
        probe_frames.push_back(VP->NumFrames - 1);
    std::sort(probe_frames.begin(), probe_frames.end());
    probe_frames.erase(std::unique(probe_frames.begin(), probe_frames.end()), probe_frames.end());

    const char *hw_candidates[] = { "cuda", "d3d11va", "dxva2" };
    bool has_active_hw = false;
    for (const char *hw_name : hw_candidates) {
        FFMS_ErrorInfo hw_err;
        char hw_error_msg[1024];
        hw_err.Buffer = hw_error_msg;
        hw_err.BufferSize = sizeof(hw_error_msg);
        hw_err.ErrorType = FFMS_ERROR_SUCCESS;
        hw_err.SubType = FFMS_ERROR_SUCCESS;

        std::unique_ptr<FFMS_VideoSource, decltype(&FFMS_DestroyVideoSource)> hw_source(
            FFMS_CreateVideoSource(FilePath.c_str(), video_track_idx, index, 1, FFMS_SEEK_NORMAL, &hw_err, hw_name, 0),
            FFMS_DestroyVideoSource
        );
        if (!hw_source)
            continue;

        const FFMS_VideoProperties *hw_vp = FFMS_GetVideoProperties(hw_source.get());
        ASSERT_NE(nullptr, hw_vp);
        if (!hw_vp->HardwareDecodeActive)
            continue;

        has_active_hw = true;
        for (int frame_num : probe_frames) {
            std::stringstream ss;
            ss << "HW=" << hw_name << ", Frame=" << frame_num;
            SCOPED_TRACE(ss.str());

            const FFMS_FrameInfo *info = FFMS_GetFrameInfo(track, frame_num);
            ASSERT_NE(nullptr, info);

            const FFMS_Frame *sw_frame = FFMS_GetFrame(video_source, frame_num, &E);
            ASSERT_NE(nullptr, sw_frame);

            FFMS_ErrorInfo hw_frame_err;
            char hw_frame_error_msg[1024];
            hw_frame_err.Buffer = hw_frame_error_msg;
            hw_frame_err.BufferSize = sizeof(hw_frame_error_msg);
            hw_frame_err.ErrorType = FFMS_ERROR_SUCCESS;
            hw_frame_err.SubType = FFMS_ERROR_SUCCESS;

            const FFMS_Frame *hw_frame = FFMS_GetFrame(hw_source.get(), frame_num, &hw_frame_err);
            ASSERT_NE(nullptr, hw_frame);

            // The same frame number should map to identical frame role metadata.
            EXPECT_EQ(hw_frame->KeyFrame, sw_frame->KeyFrame);
            EXPECT_EQ(hw_frame->RepeatPict, sw_frame->RepeatPict);
            EXPECT_EQ(hw_frame->PictType, sw_frame->PictType);
            EXPECT_EQ(hw_frame->EncodedWidth, sw_frame->EncodedWidth);
            EXPECT_EQ(hw_frame->EncodedHeight, sw_frame->EncodedHeight);
            EXPECT_EQ(hw_frame->KeyFrame, info->KeyFrame);
            EXPECT_EQ(hw_frame->RepeatPict, info->RepeatPict);
        }
    }

    if (!has_active_hw)
        GTEST_SKIP() << "No active hardware decoder path available on this machine.";
}

INSTANTIATE_TEST_CASE_P(ValidateIndexer, IndexerTest, ::testing::ValuesIn(TestFiles));

} //namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
