/*
 *  Copyright (C) 2021 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "../CompKodiProps.h"
#include "../SrvBroker.h"
#include "../decrypters/Helpers.h"
#include "../utils/Base64Utils.h"
#include "../utils/UrlUtils.h"
#include "../utils/Utils.h"
#include "TestHelper.h"

#include <gtest/gtest.h>

using namespace UTILS;

class DASHTreeTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_reprChooser = new CTestRepresentationChooserDefault();
    tree = new DASHTestTree();
  }

  void TearDown() override
  {
    tree->Uninitialize();
    testHelper::effectiveUrl.clear();
    delete tree;
    tree = nullptr;
    delete m_reprChooser;
    m_reprChooser = nullptr;
  }

  void OpenTestFile(std::string filePath) { OpenTestFile(filePath, "http://foo.bar/" + filePath); }

  void OpenTestFile(std::string filePath, std::string url) { OpenTestFile(filePath, url, {}, ""); }

  void OpenTestFile(std::string filePath, std::string url, std::map<std::string, std::string> manifestHeaders)
  {
    OpenTestFile(filePath, url, manifestHeaders, "");
  }

  void OpenTestFile(std::string filePath,
                    std::string url,
                    std::map<std::string, std::string> manifestHeaders,
                    std::string manifestUpdParams)
  {
    testHelper::testFile = filePath;

    CSrvBroker::GetInstance()->Init(m_kodiProps);

    // Download the manifest
    UTILS::CURL::HTTPResponse resp;
    if (!testHelper::DownloadFile(url, {}, {}, resp))
    {
      LOG::Log(LOGERROR, "Cannot download \"%s\" DASH manifest file.", url.c_str());
      exit(1);
    }

    ADP::KODI_PROPS::ChooserProps chooserProps;
    m_reprChooser->Initialize(chooserProps);
    // We set the download speed to calculate the initial network bandwidth
    m_reprChooser->SetDownloadSpeed(500000);

    tree->Configure(m_reprChooser, std::vector<std::string_view>{DRM::URN_WIDEVINE},
                    manifestUpdParams);

    // Parse the manifest
    if (!tree->Open(resp.effectiveUrl, resp.headers, resp.data))
    {
      LOG::Log(LOGERROR, "Cannot open \"%s\" DASH manifest.", url.c_str());
      exit(1);
    }
    tree->PostOpen();
  }

  // To set custom Kodi properties, must be called before OpenTestFile method
  void SetKodiProps(bool isPlayTimeshift)
  {
    m_kodiProps.emplace("inputstream.adaptive.play_timeshift_buffer", isPlayTimeshift ? "true" : "false");
  }

  DASHTestTree* tree;
  CHOOSER::IRepresentationChooser* m_reprChooser{nullptr};
  std::map<std::string, std::string> m_kodiProps;
};

class DASHTreeAdaptiveStreamTest : public DASHTreeTest
{
protected:
  void SetUp() override
  {
    DASHTreeTest::SetUp();
  }

  void TearDown() override
  {
    delete testStream;
    DASHTreeTest::TearDown();
  }

  void SetTestStream(TestAdaptiveStream* newStream)
  {
    delete testStream;
    testHelper::downloadList.clear();
    testStream = newStream;
  }

  TestAdaptiveStream* NewStream(PLAYLIST::CAdaptationSet* adp)
  {
    return NewStream(adp, nullptr);
  }

  TestAdaptiveStream* NewStream(PLAYLIST::CAdaptationSet* adp, PLAYLIST::CRepresentation* repr)
  {
    PLAYLIST::CRepresentation* initialRepr = repr;
    if (!repr)
      initialRepr = tree->GetRepChooser()->GetRepresentation(adp);

    return new TestAdaptiveStream(tree, adp, initialRepr);
  }

  void ReadSegments(TestAdaptiveStream* stream,
                    uint32_t bytesToRead,
                    uint32_t reads)
  {
    // Rudimentary simulation of running a stream and consuming segment data.
    // Normally AdaptiveStream::read is called from a sample reader for the exact
    // amount of bytes needed to supply the next sample until the segment is
    // exhausted. Here our segments are a fixed size (16 bytes) and for testing we can
    // optimally call to read 1 segment per AdaptiveStream::read
    bool ret;
    for (unsigned int i = 0; i < reads; i++)
    {
      ret = stream->read(&buf, bytesToRead);
      // prevent race condition leading to deadlock
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (!ret)
        break;
    }

    // Decrement last updated time so live manifest will always refresh on each segment
    // in order to test manifest update changes
    tree->SetLastUpdated(std::chrono::system_clock::now() - std::chrono::seconds(2));
    stream->SetLastUpdated(std::chrono::system_clock::now() - std::chrono::seconds(2));
  }

  TestAdaptiveStream* testStream = nullptr;
  TestAdaptiveStream* newStream = nullptr;
  std::vector<std::string> downloadedUrls;
  std::map<std::string, std::string> mediaHeaders;
  unsigned char buf[16];
};

TEST_F(DASHTreeTest, CalculateBaseURL)
{
  // No BaseURL tags
  OpenTestFile("mpd/segtpl.mpd", "https://foo.bar/mpd/test.mpd");
  EXPECT_EQ(tree->base_url_, "https://foo.bar/mpd/");
}

TEST_F(DASHTreeTest, CalculateBaseUrlFromRedirect)
{
  testHelper::effectiveUrl = "https://foo.bar/mpd/stream.mpd";
  OpenTestFile("mpd/segtpl.mpd", "https://bit.ly/abcd.mpd");
  EXPECT_EQ(tree->base_url_, "https://foo.bar/mpd/");
  EXPECT_EQ(tree->manifest_url_, "https://foo.bar/mpd/stream.mpd");
}

TEST_F(DASHTreeTest, CalculateBaseURLFromBaseURLTag)
{
  OpenTestFile("mpd/segtpl_baseurlinmpd.mpd", "https://bit.ly/abcd");
  EXPECT_EQ(tree->m_currentPeriod->GetBaseUrl(), "https://foo.bar/mpd/");
}

TEST_F(DASHTreeAdaptiveStreamTest, CalculateBaseURLWithNoSlashOutsidePeriod)
{
  SetKodiProps(true);

  // BaseURL outside period with no trailing slash
  OpenTestFile("mpd/segtpl_baseurl_noslash_outside.mpd", "https://bit.ly/abcd");

  EXPECT_EQ(tree->m_currentPeriod->GetBaseUrl(), "https://foo.bar/mpd/");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[0].get()));

  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/mpd/V300/init.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/mpd/V300/4999850.m4s");
}

TEST_F(DASHTreeAdaptiveStreamTest, CalculateSegTplWithNoSlashes)
{
  // BaseURL inside period with no trailing slash, uses segtpl, media/init doesn't start with slash
  OpenTestFile("mpd/segtpl_baseurl_noslashs.mpd", "https://foo.bar/initialpath/test.mpd");

  EXPECT_EQ(tree->m_currentPeriod->GetBaseUrl(), "https://foo.bar/guid.ism/dash/");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[0].get()));

  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/guid.ism/dash/media-video=66000.dash");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/guid.ism/dash/media-video=66000-1.m4s");
}

TEST_F(DASHTreeAdaptiveStreamTest, CalculateSegTplWithMediaInitSlash)
{
  // BaseURL inside period with no trailing slash, uses segtpl, media/init starts with slash
  OpenTestFile("mpd/segtpl_slash_baseurl_noslash.mpd", "https://foo.bar/initialpath/test.mpd");

  EXPECT_EQ(tree->m_currentPeriod->GetBaseUrl(), "https://foo.bar/guid.ism/dash/");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[0].get()));

  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/media-video=66000.dash");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/media-video=66000-1.m4s");
}

TEST_F(DASHTreeAdaptiveStreamTest, CalculateSegTplWithBaseURLSlash)
{
  // BaseURL inside period with trailing slash, uses segtpl, media/init doesn't start with slash
  OpenTestFile("mpd/segtpl_noslash_baseurl_slash.mpd", "https://foo.bar/initialpath/test.mpd");

  EXPECT_EQ(tree->m_currentPeriod->GetBaseUrl(), "https://foo.bar/guid.ism/dash/");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[0].get()));

  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/guid.ism/dash/media-video=66000.dash");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/guid.ism/dash/media-video=66000-1.m4s");
}

TEST_F(DASHTreeAdaptiveStreamTest, CalculateSegTplWithBaseURLAndMediaInitSlash)
{
  // BaseURL inside period with trailing slash, uses segtpl, media/init starts with slash
  OpenTestFile("mpd/segtpl_slash_baseurl_slash.mpd", "https://foo.bar/initialpath/test.mpd");

  EXPECT_EQ(tree->m_currentPeriod->GetBaseUrl(), "https://foo.bar/guid.ism/dash/");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[0].get()));

  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/media-video=66000.dash");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/media-video=66000-1.m4s");
}

TEST_F(DASHTreeTest, CalculateBaseURLInRepRangeBytes)
{
  // Byteranged indexing
  OpenTestFile("mpd/segmentbase.mpd", "https://foo.bar/test.mpd");
  EXPECT_EQ(tree->m_periods[0]->GetAdaptationSets()[0]->GetRepresentations()[0]->GetBaseUrl(),
            "https://foo.bar/video/23.98p/r0/vid10.mp4");
}

TEST_F(DASHTreeTest, CalculateCorrectSegmentNumbersFromSegmentTimeline)
{
  // SegmentTimeline, availabilityStartTime is greater than epoch
  OpenTestFile("mpd/segtimeline_live_ast.mpd");

  auto& segments =
      tree->m_periods[0]->GetAdaptationSets()[0]->GetRepresentations()[0]->Timeline();

  EXPECT_EQ(segments.GetSize(), 13);
  EXPECT_EQ(segments.Get(0)->m_number, 487050);
  EXPECT_EQ(segments.Get(12)->m_number, 487062);
}

TEST_F(DASHTreeTest, CalculateCorrectSegmentNumbersFromSegmentTemplateWithPTO)
{
  tree->SetNowTime(1617223929000);

  OpenTestFile("mpd/segtpl_pto.mpd");

  auto& segments = tree->m_periods[0]->GetAdaptationSets()[0]->GetRepresentations()[0]->Timeline();

  EXPECT_EQ(segments.GetSize(), 450);
  EXPECT_EQ(segments.Get(0)->m_number, 404305524);
}

TEST_F(DASHTreeTest, CalculateCorrectSegmentNumbersFromSegmentTemplateWithOldPublishTime)
{
  tree->SetNowTime(1617229334000);

  OpenTestFile("mpd/segtpl_old_publish_time.mpd");

  auto& segments = tree->m_periods[0]->GetAdaptationSets()[0]->GetRepresentations()[0]->Timeline();

  EXPECT_EQ(segments.GetSize(), 30);
  EXPECT_EQ(segments.Get(0)->m_number, 603271);
}

TEST_F(DASHTreeTest, CalculateCorrectFpsScaleFromAdaptionSet)
{
  OpenTestFile("mpd/fps_scale_adaptset.mpd");

  auto& adpSets = tree->m_periods[0]->GetAdaptationSets();

  EXPECT_EQ(adpSets[0]->GetRepresentations()[0]->GetFrameRate(), 24000);
  EXPECT_EQ(adpSets[0]->GetRepresentations()[0]->GetFrameRateScale(), 1001);

  EXPECT_EQ(adpSets[1]->GetRepresentations()[0]->GetFrameRate(), 30);
  EXPECT_EQ(adpSets[1]->GetRepresentations()[0]->GetFrameRateScale(), 1);

  EXPECT_EQ(adpSets[2]->GetRepresentations()[0]->GetFrameRate(), 25);
  EXPECT_EQ(adpSets[2]->GetRepresentations()[0]->GetFrameRateScale(), 1);

  EXPECT_EQ(adpSets[3]->GetRepresentations()[0]->GetFrameRate(), 25000);
  EXPECT_EQ(adpSets[3]->GetRepresentations()[0]->GetFrameRateScale(), 1000);

  EXPECT_EQ(adpSets[4]->GetRepresentations()[0]->GetFrameRate(), 25);
  EXPECT_EQ(adpSets[4]->GetRepresentations()[0]->GetFrameRateScale(), 1);

  EXPECT_EQ(adpSets[5]->GetRepresentations()[0]->GetFrameRate(), 30);
  EXPECT_EQ(adpSets[5]->GetRepresentations()[0]->GetFrameRateScale(), 1);

  EXPECT_EQ(adpSets[6]->GetRepresentations()[0]->GetFrameRate(), 25000);
  EXPECT_EQ(adpSets[6]->GetRepresentations()[0]->GetFrameRateScale(), 1000);
}

TEST_F(DASHTreeAdaptiveStreamTest, replacePlaceHolders)
{
  SetKodiProps(true);
  OpenTestFile("mpd/placeholders.mpd", "https://foo.bar/placeholders.mpd");
  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[0].get()));

  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/videosd-400x224/init.mp4");
  EXPECT_EQ(testHelper::downloadList[4], "https://foo.bar/videosd-400x224/segment_487053.m4s");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[1].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/videosd-400x224/segment_00487050.m4s");
  EXPECT_EQ(testHelper::downloadList[4], "https://foo.bar/videosd-400x224/segment_00487053.m4s");
  
  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[2].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/videosd-400x224/segment_263007000000.m4s");
  EXPECT_EQ(testHelper::downloadList[4], "https://foo.bar/videosd-400x224/segment_263008620000.m4s");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[3].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/videosd-400x224/segment_00263007000000");
  EXPECT_EQ(testHelper::downloadList[4], "https://foo.bar/videosd-400x224/segment_00263008620000");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[4].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/videosd-400x224/segment_487050.m4s?t=263007000000");
  EXPECT_EQ(testHelper::downloadList[4], "https://foo.bar/videosd-400x224/segment_487053.m4s?t=263008620000");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[5].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/videosd-400x224/segment_00487050.m4s?t=00263007000000");
  EXPECT_EQ(testHelper::downloadList[4], "https://foo.bar/videosd-400x224/segment_00487053.m4s?t=00263008620000");

  SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[6].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/videosd-400x224/init.mp4");
  EXPECT_EQ(testHelper::downloadList[4], "https://foo.bar/videosd-400x224/segment.m4s");
}

TEST_F(DASHTreeTest, isLiveManifestOnLiveSegmentTimeline)
{
  OpenTestFile("mpd/segtimeline_live_pd.mpd");
  EXPECT_EQ(tree->IsLive(), true);
}

TEST_F(DASHTreeTest, isLiveManifestOnVODSegmentTimeline)
{
  OpenTestFile("mpd/segtimeline_vod.mpd");
  EXPECT_EQ(tree->IsLive(), false);
}

TEST_F(DASHTreeTest, CalculatePsshDefaultKid)
{
  OpenTestFile("mpd/pssh_default_kid.mpd");

  const std::vector<uint8_t> pssh1 = BASE64::Decode("AAAANHBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAABQIARIQblodJidXR9eARuql0dNLWg==");
  EXPECT_EQ(tree->m_periods[0]->GetPSSHSets()[1].pssh_, pssh1);
  // The following KID on manifest is represented as UUID and dashes must be deleted (string size 36 to 32)
  EXPECT_EQ(tree->m_periods[0]->GetPSSHSets()[1].defaultKID_.size(), 32);

  const std::vector<uint8_t> pssh2 = BASE64::Decode("AAAANHBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAABQIARIQnrQFDeRLSAKTLifXUIPiZg==");
  EXPECT_EQ(tree->m_periods[0]->GetPSSHSets()[2].pssh_, pssh2);
  EXPECT_EQ(tree->m_periods[0]->GetPSSHSets()[2].defaultKID_.size(), 32);
}

// A seek to a time in the current segment restarts the segment, also a seek
// to a time before the last read. A reader cannot seek back by itself.
TEST_F(DASHTreeAdaptiveStreamTest, SeekInTheCurrentSegmentRestartsTheSegment)
{
  OpenTestFile("mpd/segtpl.mpd", "https://foo.bar/mpd/test.mpd");
  for (const bool preceeding : {true, false})
  {
    SCOPED_TRACE(preceeding ? "preceeding" : "not preceeding");
    SetTestStream(NewStream(tree->m_periods[0]->GetAdaptationSets()[0].get()));
    testStream->start_stream();
    // The initialization segment, then a part of the first media segment
    ASSERT_EQ(testStream->read(buf, 16), 16U);
    ASSERT_EQ(testStream->read(buf, 4), 4U);
    const uint64_t segmentStart{testStream->tell() - 4};
    const PLAYLIST::CRepresentation* rep{testStream->getRepresentation()};
    const PLAYLIST::CSegment* segment{rep->current_segment_};
    ASSERT_NE(segment, nullptr);
    // A video seek that is not preceeding takes the next segment after a segment start
    const double seconds{static_cast<double>(segment->startPTS_) / rep->GetTimescale()};
    bool needReset{false};
    ASSERT_TRUE(testStream->seek_time(seconds, preceeding, needReset));
    EXPECT_TRUE(needReset);
    EXPECT_EQ(rep->current_segment_, segment);
    EXPECT_EQ(testStream->tell(), segmentStart);
    ASSERT_EQ(testStream->read(buf, 4), 4U);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), 4), "Sixt");
  }
}

TEST_F(DASHTreeAdaptiveStreamTest, subtitles)
{
  OpenTestFile("mpd/subtitles.mpd", "https://foo.bar/subtitles.mpd");

  auto& adpSets = tree->m_periods[0]->GetAdaptationSets();

  EXPECT_EQ(adpSets[1]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[1]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[1]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_TTML), true);
  EXPECT_EQ(adpSets[1]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[2]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[2]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[2]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_TTML), true);
  EXPECT_EQ(adpSets[2]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[3]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[3]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[3]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_TTML), true);
  EXPECT_EQ(adpSets[3]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[4]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[4]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[4]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_TTML), true);
  EXPECT_EQ(adpSets[4]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[5]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[5]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[5]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_WVTT), true);
  EXPECT_EQ(adpSets[5]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[6]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[6]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[6]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_WVTT), true);
  EXPECT_EQ(adpSets[6]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[7]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[7]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[7]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_WVTT), true);
  EXPECT_EQ(adpSets[7]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[8]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[8]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[8]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_WVTT), true);
  EXPECT_EQ(adpSets[8]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[9]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[9]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[9]->GetRepresentations()[0]->GetCodecs(), "my_codec"), true);
  EXPECT_EQ(adpSets[9]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[10]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(adpSets[10]->GetRepresentations()[0]->IsSubtitleFileStream(), true);
  EXPECT_EQ(CODEC::Contains(adpSets[10]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_TTML), true);
  EXPECT_EQ(adpSets[10]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::TEXT);

  EXPECT_EQ(adpSets[11]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(STR(adpSets[11]->GetRepresentations()[0]->GetMimeType()), "application/mp4");
  EXPECT_EQ(CODEC::Contains(adpSets[11]->GetRepresentations()[0]->GetCodecs(), CODEC::FOURCC_STPP), true);
  EXPECT_EQ(adpSets[11]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::MP4);

  SetTestStream(NewStream(adpSets[11].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/11/init.mp4");
  EXPECT_EQ(testHelper::downloadList[4], "https://foo.bar/11/0004.m4s");

  EXPECT_EQ(adpSets[12]->GetStreamType(), PLAYLIST::StreamType::SUBTITLE);
  EXPECT_EQ(STR(adpSets[12]->GetMimeType()), "application/mp4");
  EXPECT_EQ(CODEC::Contains(adpSets[12]->GetRepresentations()[0]->GetCodecs(), "stpp.ttml.im1t"), true);
  EXPECT_EQ(adpSets[12]->GetRepresentations()[0]->GetContainerType(), PLAYLIST::ContainerType::MP4);

  SetTestStream(NewStream(adpSets[12].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/tears-of-steel-multiple-subtitles-12.dash");
  EXPECT_EQ(testHelper::downloadList[4], "https://foo.bar/tears-of-steel-multiple-subtitles-12-12000.dash");
}

TEST_F(DASHTreeTest, CalculateMultipleSegTpl)
{
  OpenTestFile("mpd/segtpl_multiple.mpd", "https://foo.bar/dash/multiple.mpd");

  EXPECT_EQ(tree->base_url_, "https://foo.bar/dash/");

  auto& adpSets = tree->m_periods[0]->GetAdaptationSets();

  EXPECT_EQ(STR(adpSets[0]->GetRepresentations()[0]->GetSegmentTemplate()->GetInitialization()), "3c1055cb-a842-4449-b393-7f31693b4a8f_1_448x252init.mp4");
  EXPECT_EQ(STR(adpSets[0]->GetRepresentations()[0]->GetSegmentTemplate()->GetMedia()), "3c1055cb-a842-4449-b393-7f31693b4a8f_1_448x252_$Number%09d$.mp4");
  EXPECT_EQ(adpSets[0]->GetRepresentations()[0]->GetSegmentTemplate()->GetTimescale(), 120000);
  EXPECT_EQ(adpSets[0]->GetRepresentations()[0]->Timeline().Get(0)->m_number, 3);

  EXPECT_EQ(STR(adpSets[0]->GetRepresentations()[1]->GetSegmentTemplate()->GetInitialization()), "3c1055cb-a842-4449-b393-7f31693b4a8f_2_1920x1080init.mp4");
  EXPECT_EQ(STR(adpSets[0]->GetRepresentations()[1]->GetSegmentTemplate()->GetMedia()), "3c1055cb-a842-4449-b393-7f31693b4a8f_2_1920x1080_$Number%09d$.mp4");
  EXPECT_EQ(adpSets[0]->GetRepresentations()[1]->GetSegmentTemplate()->GetTimescale(), 90000);
  EXPECT_EQ(adpSets[0]->GetRepresentations()[1]->Timeline().Get(0)->m_number, 5);

  EXPECT_EQ(STR(adpSets[1]->GetRepresentations()[0]->GetSegmentTemplate()->GetInitialization()), "3c1055cb-a842-4449-b393-7f31693b4a8f_aac1init.mp4");
  EXPECT_EQ(STR(adpSets[1]->GetRepresentations()[0]->GetSegmentTemplate()->GetMedia()), "3c1055cb-a842-4449-b393-7f31693b4a8f_aac1_$Number%09d$.mp4");
  EXPECT_EQ(adpSets[1]->GetRepresentations()[0]->GetSegmentTemplate()->GetTimescale(), 48000);
  EXPECT_EQ(adpSets[1]->GetRepresentations()[0]->Timeline().Get(0)->m_number, 1);

  EXPECT_EQ(STR(adpSets[2]->GetRepresentations()[0]->GetSegmentTemplate()->GetInitialization()), "abc_aac1init.mp4");
  EXPECT_EQ(STR(adpSets[2]->GetRepresentations()[0]->GetSegmentTemplate()->GetMedia()), "abc2_$Number%09d$.mp4");
  EXPECT_EQ(adpSets[2]->GetRepresentations()[0]->GetSegmentTemplate()->GetTimescale(), 68000);
  EXPECT_EQ(adpSets[2]->GetRepresentations()[0]->Timeline().Get(0)->m_number, 5);
}

TEST_F(DASHTreeAdaptiveStreamTest, CalculateRedirectSegTpl)
{
  SetKodiProps(true);
  testHelper::effectiveUrl = "https://foo.bar/mpd/stream.mpd";
  OpenTestFile("mpd/segtpl.mpd", "https://bit.ly/abcd.mpd");

  auto& adpSets = tree->m_periods[0]->GetAdaptationSets();

  SetTestStream(NewStream(adpSets[0].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);

  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/mpd/V300/init.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/mpd/V300/4999850.m4s");

  SetTestStream(NewStream(adpSets[1].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/A48/init.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/A48/4999850.m4s");
}

TEST_F(DASHTreeAdaptiveStreamTest, CalculateReprensentationBaseURL)
{
  SetKodiProps(true);
  OpenTestFile("mpd/rep_base_url.mpd", "https://bit.ly/mpd/abcd.mpd");

  auto& adpSets = tree->m_periods[0]->GetAdaptationSets();
  auto adpSet0 = adpSets[0].get();

  SetTestStream(NewStream(adpSet0, adpSet0->GetRepresentations()[0].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/mpd/slices/A_init.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/mpd/slices/A00000714.m4f");

  SetTestStream(NewStream(adpSet0, adpSet0->GetRepresentations()[1].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://bit.ly/mpd/B_init.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://bit.ly/mpd/B00000714.m4f");

  auto adpSet1 = adpSets[1].get();

  SetTestStream(NewStream(adpSet1, adpSet1->GetRepresentations()[0].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/mpd/slices/A_init.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/mpd/slices/A00000714.m4f");

  SetTestStream(NewStream(adpSet1, adpSet1->GetRepresentations()[1].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/mpd/slices2/B_init.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/mpd/slices2/B00000714.m4f");

  SetTestStream(NewStream(adpSet1, adpSet1->GetRepresentations()[2].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://foo.bar/mpd/slices2/C_init.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://foo.bar/mpd/slices2/C00000714.m4f");
}

TEST_F(DASHTreeAdaptiveStreamTest, CalculateReprensentationBaseURLMultiple)
{
  OpenTestFile(
      "mpd/rep_base_url_multiple.mpd",
      "https://pl.foobar.com/assets/p/c30668ab1d7d10166938f06b9643a254.urlset/manifest.mpd");

  auto& adpSets = tree->m_periods[0]->GetAdaptationSets();

  SetTestStream(NewStream(adpSets[0].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);

  EXPECT_EQ(testHelper::downloadList[0], "https://prod.foobar.com/video/assets/p/c30668ab1d7d10166938f06b9643a254.urlset/init-f1-v1-x3.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://prod.foobar.com/video/assets/p/c30668ab1d7d10166938f06b9643a254.urlset/fragment-1-f1-v1-x3.m4s");

  SetTestStream(NewStream(adpSets[1].get()));
  testStream->start_stream();
  ReadSegments(testStream, 16, 5);
  EXPECT_EQ(testHelper::downloadList[0], "https://prod.foobar.com/audio/assets/p/c30668ab1d7d10166938f06b9643a254.urlset/init-f1-a1-x3.mp4");
  EXPECT_EQ(testHelper::downloadList[1], "https://prod.foobar.com/audio/assets/p/c30668ab1d7d10166938f06b9643a254.urlset/fragment-1-f1-a1-x3.m4s");
}

TEST_F(DASHTreeAdaptiveStreamTest, MisalignedSegmentTimeline)
{
  OpenTestFile("mpd/bad_segtimeline_1.mpd", "https://foo.bar/placeholders.mpd");

  auto& repr = tree->m_currentPeriod->GetAdaptationSets()[1]->GetRepresentations()[0];
  // Set the last segment to the current segment to simulate reaching the last segment
  repr->current_segment_ = repr->Timeline().GetBack();

  EXPECT_EQ(repr->current_segment_->startPTS_, 95687379264);
  EXPECT_EQ(repr->Timeline().GetPos(repr->current_segment_), 4);

  tree->RunManifestUpdate("mpd/bad_segtimeline_2.mpd");
  EXPECT_EQ(repr->current_segment_->startPTS_, 95687381280);
  EXPECT_EQ(repr->Timeline().GetPos(repr->current_segment_), 2);

  tree->RunManifestUpdate("mpd/bad_segtimeline_3.mpd");
  EXPECT_EQ(repr->current_segment_->startPTS_, 95687382336);
  EXPECT_EQ(repr->Timeline().GetPos(repr->current_segment_), 1);

  tree->RunManifestUpdate("mpd/bad_segtimeline_4.mpd");
  EXPECT_EQ(repr->current_segment_->startPTS_, 95687382337);
  EXPECT_EQ(repr->Timeline().GetPos(repr->current_segment_), 0);
}

TEST_F(DASHTreeTest, AdaptionSetSwitching)
{
  OpenTestFile("mpd/adaptation_set_switching.mpd");

  auto& adpSets = tree->m_periods[0]->GetAdaptationSets();

  EXPECT_EQ(adpSets.size(), 6);
  EXPECT_EQ(STR(adpSets[0]->GetRepresentations()[0]->GetId()), "3");
  EXPECT_EQ(STR(adpSets[0]->GetRepresentations()[1]->GetId()), "1");
  EXPECT_EQ(STR(adpSets[0]->GetRepresentations()[2]->GetId()), "2");
  // Below adaptation set (id 6) should be merged with previous one
  // but since has a different codec will not be merged
  // see note on related DASH parser code
  EXPECT_EQ(STR(adpSets[1]->GetRepresentations()[0]->GetId()), "4");

  EXPECT_EQ(STR(adpSets[2]->GetRepresentations()[0]->GetId()), "5");
  EXPECT_EQ(STR(adpSets[2]->GetRepresentations()[1]->GetId()), "6");

  EXPECT_EQ(STR(adpSets[3]->GetRepresentations()[0]->GetId()), "7");

  EXPECT_EQ(STR(adpSets[4]->GetRepresentations()[0]->GetId()), "8");

  EXPECT_EQ(STR(adpSets[5]->GetRepresentations()[0]->GetId()), "9");
}

TEST_F(DASHTreeTest, AdaptionSetMerge)
{
  OpenTestFile("mpd/adaptation_set_merge.mpd");

  auto& adpSets = tree->m_periods[0]->GetAdaptationSets();

  EXPECT_EQ(adpSets.size(), 6);
  EXPECT_EQ(STR(adpSets[0]->GetRepresentations()[0]->GetId()), "video=100000");
  EXPECT_EQ(STR(adpSets[1]->GetRepresentations()[0]->GetId()), "audio_ja-JP_3=128000");
  EXPECT_EQ(STR(adpSets[2]->GetRepresentations()[0]->GetId()), "audio_es-419_3=128000");
  EXPECT_EQ(STR(adpSets[3]->GetRepresentations()[0]->GetId()), "audio_en-GB_3=96000");
  EXPECT_EQ(STR(adpSets[4]->GetRepresentations()[0]->GetId()), "audio_es-ES=20000");
  // Below two adaptation sets merged
  EXPECT_EQ(STR(adpSets[5]->GetRepresentations()[0]->GetId()), "audio_es-ES_1=64000");
  EXPECT_EQ(STR(adpSets[5]->GetRepresentations()[1]->GetId()), "audio_es-ES_1=64000"); 
}

TEST_F(DASHTreeTest, SuggestedPresentationDelay)
{
  OpenTestFile("mpd/segtpl_spd.mpd", "https://foo.bar/segtpl_spd.mpd");
  EXPECT_EQ(tree->m_liveDelay, 32);
}

TEST_F(DASHTreeTest, SegmentTemplateStartNumber)
{
  OpenTestFile("mpd/segmenttemplate_startnumber.mpd", "https://vod.service.net/SGP1/highlightpost/1234567890/1/web/dash/segtpl_sn.mpd");

  auto& adpSets = tree->m_periods[0]->GetAdaptationSets();

  EXPECT_EQ(adpSets[0]->GetRepresentations()[0]->GetSegmentTemplate()->GetStartNumber(), 0);
  EXPECT_EQ(adpSets[0]->GetRepresentations()[0]->GetSegmentTemplate()->GetTimescale(), 25000);
  EXPECT_EQ(adpSets[0]->GetRepresentations()[0]->GetSegmentTemplate()->GetDuration(), 48000);

  // Verify segments
  auto& rep1Timeline = adpSets[0]->GetRepresentations()[0]->Timeline();
  EXPECT_EQ(rep1Timeline.GetSize(), 144);

  EXPECT_EQ(rep1Timeline.Get(0)->startPTS_, 0);
  EXPECT_EQ(rep1Timeline.Get(0)->m_number, 0);

  EXPECT_EQ(rep1Timeline.Get(1)->startPTS_, 48000);
  EXPECT_EQ(rep1Timeline.Get(1)->m_number, 1);

  EXPECT_EQ(rep1Timeline.Get(143)->startPTS_, 6864000);
  EXPECT_EQ(rep1Timeline.Get(143)->m_number, 143);
}

TEST_F(DASHTreeTest, TSBMiddlePeriods)
{
  // At playback start the TSB fall in the middle of the two periods
  // TSB segments must be generated for the second period
  tree->SetNowTime(1712130845000);

  OpenTestFile("mpd/tsb_middle_periods.mpd");

  auto& tlPeriod1 =
      tree->m_periods[0]->GetAdaptationSets()[0]->GetRepresentations()[0]->Timeline();

  EXPECT_EQ(tlPeriod1.GetSize(), 90);
  EXPECT_EQ(tlPeriod1.GetFront()->m_number, 856065330);
  EXPECT_EQ(tlPeriod1.GetBack()->m_number, 856065419);

  auto& tlPeriod2 =
      tree->m_periods[1]->GetAdaptationSets()[0]->GetRepresentations()[0]->Timeline();

  EXPECT_EQ(tlPeriod2.GetSize(), 2);
  EXPECT_EQ(tlPeriod2.GetFront()->m_number, 856065420);
  EXPECT_EQ(tlPeriod2.GetBack()->m_number, 856065421);
}

TEST_F(DASHTreeTest, TSBMiddlePeriodsPastNowTime)
{
  // Simulate a playback that has already started with a past NOW time (MPD update is assumed)
  // no TSB segments must be generated on the periods.
  // 1 segment is generated by default on second period to allow new segments to be inserted during live.
  tree->SetNowTime(1712030845000);

  OpenTestFile("mpd/tsb_middle_periods.mpd");

  auto& tlPeriod1 =
      tree->m_periods[0]->GetAdaptationSets()[0]->GetRepresentations()[0]->Timeline();

  EXPECT_EQ(tlPeriod1.GetSize(), 90);
  EXPECT_EQ(tlPeriod1.GetFront()->m_number, 856065330);
  EXPECT_EQ(tlPeriod1.GetBack()->m_number, 856065419);

  auto& tlPeriod2 =
      tree->m_periods[1]->GetAdaptationSets()[0]->GetRepresentations()[0]->Timeline();

  EXPECT_EQ(tlPeriod2.GetSize(), 1);
  EXPECT_EQ(tlPeriod2.GetFront()->m_number, 856065420);
  EXPECT_EQ(tlPeriod2.GetBack()->m_number, 856065420);
}

TEST_F(DASHTreeTest, TSBAvailabilityStartTime)
{
  // The TSB of 2h with MPD availabilityStartTime to be taken in to account
  tree->SetNowTime(1712130845000);

  OpenTestFile("mpd/tsb_availstarttime.mpd");

  auto& tl =
      tree->m_periods[0]->GetAdaptationSets()[0]->GetRepresentations()[0]->Timeline();

  EXPECT_EQ(tl.GetSize(), 1200);
  EXPECT_EQ(tl.GetFront()->m_number, 129069);
  EXPECT_EQ(tl.GetBack()->m_number, 130268);
}

TEST_F(DASHTreeTest, LastSegmentOfEachPeriod)
{
  // Three periods of 6 s with segments of 2 s, from a SegmentTimeline, a
  // SegmentList, a SegmentTemplate duration or the index of a SegmentBase. In a
  // live stream, the last period continues, and an earlier period can still be
  // in progress (two segments). A presentationTimeOffset moves the SegmentTimeline
  // times above or below the period start. An update period of 0 updates the
  // manifest at each segment. Without manifest updates, no segment follows the
  // timeline.
  enum class Segments
  {
    TIMELINE,
    LIST,
    DURATION,
    BASE
  };
  for (const Segments mode :
       {Segments::TIMELINE, Segments::LIST, Segments::DURATION, Segments::BASE})
  {
    for (const bool live : {false, true})
    {
      for (const uint64_t offset : {uint64_t{0}, uint64_t{2}, uint64_t{100}})
      {
        for (const std::string update : {"PT2S", "PT0S", ""})
        {
          for (const int partial : {-1, 0, 1})
          {
            // A SegmentTemplate duration fills the period
            if (mode == Segments::DURATION && partial >= 0)
              continue;
            SCOPED_TRACE(testing::Message()
                         << "segments=" << static_cast<int>(mode) << " live=" << live
                         << " presentationTimeOffset=" << offset
                         << " minimumUpdatePeriod=" << update << " partial period=" << partial);
            tree->Uninitialize();
            delete tree;
            tree = new DASHTestTree();
            CSrvBroker::GetInstance()->Init(m_kodiProps);
            tree->Configure(m_reprChooser, {}, "");
            const std::string pto{
                offset ? " presentationTimeOffset=\"" + std::to_string(offset) + "\"" : ""};
            std::string manifest{"<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"" +
                                 std::string{live ? "dynamic" : "static"} + "\"" +
                                 (update.empty() ? "" : " minimumUpdatePeriod=\"" + update + "\"") +
                                 ">"};
            for (int period = 0; period < 3; ++period)
            {
              const int segments{period == partial ? 2 : 3};
              // The segments of a SegmentTemplate are in the adaptation set, the
              // others in the representation
              std::string adaptationInfo;
              std::string representationInfo;
              if (mode == Segments::TIMELINE)
                adaptationInfo = "<SegmentTemplate timescale=\"1\" media=\"seg-$Number$.ts\"" +
                                 pto + "><SegmentTimeline><S t=\"" + std::to_string(offset) +
                                 "\" d=\"2\" r=\"" + std::to_string(segments - 1) +
                                 "\"/></SegmentTimeline></SegmentTemplate>";
              else if (mode == Segments::DURATION)
                adaptationInfo = "<SegmentTemplate timescale=\"1\" duration=\"2\" "
                                 "startNumber=\"1\" media=\"seg-$Number$.ts\"" +
                                 pto + "/>";
              else if (mode == Segments::LIST)
              {
                representationInfo = "<SegmentList timescale=\"1\" duration=\"2\"" + pto + ">";
                for (int segment = 0; segment < segments; ++segment)
                  representationInfo +=
                      "<SegmentURL media=\"seg-" + std::to_string(segment) + ".ts\"/>";
                representationInfo += "</SegmentList>";
              }
              else
                representationInfo =
                    "<BaseURL>media.mp4</BaseURL><SegmentBase indexRange=\"0-99\"/>";
              manifest += "<Period id=\"" + std::to_string(period) + "\" start=\"PT" +
                          std::to_string(period * 6) +
                          "S\" duration=\"PT6S\"><AdaptationSet mimeType=\"video/mp4\" "
                          "contentType=\"video\">" +
                          adaptationInfo +
                          "<Representation id=\"1\" codecs=\"avc1.4d401f\" "
                          "bandwidth=\"1000000\">" +
                          representationInfo + "</Representation></AdaptationSet></Period>";
            }
            manifest += "</MPD>";
            ASSERT_TRUE(tree->Open("http://foo.bar/periods.mpd", {}, manifest));
            ASSERT_EQ(tree->m_periods.size(), 3);
            for (int period = 0; period < 3; ++period)
            {
              tree->m_currentPeriod = tree->m_periods[period].get();
              auto adp = tree->m_currentPeriod->GetAdaptationSets()[0].get();
              auto rep = adp->GetRepresentations()[0].get();
              const int segments{period == partial ? 2 : 3};
              if (mode == Segments::BASE)
              {
                // As the index of the media file gives: times from the start of
                // the file, in the period
                rep->SetTimescale(1);
                for (int segment = 0; segment < segments; ++segment)
                {
                  PLAYLIST::CSegment seg;
                  seg.startPTS_ = period * 6 + segment * 2;
                  seg.m_endPts = seg.startPTS_ + 2;
                  rep->Timeline().Add(seg);
                }
              }
              adaptive::AdaptiveStream stream{tree, adp, rep};
              ASSERT_EQ(rep->Timeline().GetSize(), segments);
              // A live period ends with the segment at the period end
              const bool complete{!live || update.empty() || (period < 2 && period != partial)};
              for (int segment = 0; segment < segments; ++segment)
              {
                rep->current_segment_ = rep->Timeline().Get(segment);
                EXPECT_EQ(stream.IsLastSegment(), complete && segment == segments - 1)
                    << "period=" << period << " segment=" << segment;
              }
            }
          }
        }
      }
    }
  }
}

// A change of the representation ends the data of the old representation: the
// reader of the old representation returns the frames that it keeps, and the
// stream continues with the new representation after the reopen (start_stream).
// A read before the reopen read the data of the new representation into the
// reader of the old representation.
TEST_F(DASHTreeAdaptiveStreamTest, RepresentationChangeEndsTheOldData)
{
  class CAlternatingChooser : public CTestRepresentationChooserDefault
  {
  public:
    PLAYLIST::CRepresentation* GetNextRepresentation(PLAYLIST::CAdaptationSet* adp,
                                                     PLAYLIST::CRepresentation* currentRep) override
    {
      const auto& reps{adp->GetRepresentations()};
      if (!currentRep)
        return reps[0].get();
      return reps[0].get() == currentRep ? reps[1].get() : reps[0].get();
    }
  };
  class CObserver : public adaptive::AdaptiveStreamObserver
  {
  public:
    void OnSegmentChanged(adaptive::AdaptiveStream* stream) override {}
    void OnStreamChange(adaptive::AdaptiveStream* stream) override { ++m_changes; }
    int m_changes{0};
  };

  for (const bool seek : {false, true})
  {
    SCOPED_TRACE(seek ? "seek before the reopen" : "reopen");
    delete m_reprChooser;
    m_reprChooser = new CAlternatingChooser();
    tree->Uninitialize();
    delete tree;
    tree = new DASHTestTree();
    OpenTestFile("mpd/segtpl_multiple.mpd", "https://foo.bar/dash/multiple.mpd");
    PLAYLIST::CAdaptationSet* adp{tree->m_periods[0]->GetAdaptationSets()[0].get()};
    SetTestStream(NewStream(adp));
    CObserver observer;
    testStream->set_observer(&observer);
    ASSERT_TRUE(testStream->start_stream());
    const PLAYLIST::CRepresentation* first{testStream->getRepresentation()};

    // The segments of the first representation, up to the change. A read
    // fails also while the download of the next segment has not started.
    for (int reads = 0; observer.m_changes == 0; ++reads)
    {
      ASSERT_LT(reads, 50);
      testStream->read(buf, 16);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(observer.m_changes, 1);
    ASSERT_TRUE(testStream->StreamChanged());
    ASSERT_NE(testStream->getRepresentation(), first);

    // No data of the new representation before the reopen
    for (int read = 0; read < 3; ++read)
      EXPECT_EQ(testStream->read(buf, 16), 0U);
    EXPECT_EQ(observer.m_changes, 1);

    if (seek)
    {
      // The change is still pending after the seek, and the reopen continues
      // in the segment of the seek time
      bool needReset{false};
      ASSERT_TRUE(testStream->seek_time(10.0, true, needReset));
      EXPECT_EQ(testStream->read(buf, 16), 0U);
      EXPECT_TRUE(testStream->StreamChanged());
    }
    ASSERT_TRUE(testStream->start_stream());
    EXPECT_FALSE(testStream->StreamChanged());
    EXPECT_EQ(testStream->read(buf, 16), 16U);
    if (seek)
    {
      const PLAYLIST::CRepresentation* rep{testStream->getRepresentation()};
      const PLAYLIST::CSegment* segment{rep->current_segment_};
      ASSERT_NE(segment, nullptr);
      const double start{static_cast<double>(segment->startPTS_) / rep->GetTimescale()};
      const double end{static_cast<double>(segment->m_endPts) /
                       rep->GetTimescale()};
      EXPECT_LE(start, 10.0);
      EXPECT_GT(end, 10.0);
    }
    testStream->Stop();
  }
}
