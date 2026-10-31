/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AdaptiveByteStream.h"
#include "TestAdaptiveStream.h"
#include "samplereader/ADTSSampleReader.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace
{
constexpr uint64_t BASE_PTS = 9000000; // 100 s in 90 kHz ticks
constexpr int FRAMES = 200;
// Real AAC frames are larger than the 64 bytes the reader peeks to find the codec
constexpr unsigned int PAYLOAD_SIZE = 200;

// PTS of a frame in the microseconds the session passes to TimeSeek()
uint64_t FrameTime(uint64_t basePts, int frame, uint64_t samples, uint64_t rate)
{
  return (basePts + frame * samples * 90000 / rate) * 100 / 9;
}

uint64_t AacTime(int frame)
{
  return FrameTime(BASE_PTS, frame, 1024, 44100);
}

void AppendID3Timestamp(std::vector<uint8_t>& data, uint64_t pts)
{
  // ID3v2.4 tag with the HLS PRIV frame "com.apple.streaming.transportStreamTimestamp"
  const char owner[] = "com.apple.streaming.transportStreamTimestamp";
  const uint8_t header[] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 63};
  const uint8_t frame[] = {'P', 'R', 'I', 'V', 0, 0, 0, 53, 0, 0};
  data.insert(data.end(), header, header + sizeof(header));
  data.insert(data.end(), frame, frame + sizeof(frame));
  data.insert(data.end(), owner, owner + sizeof(owner));
  for (int shift = 56; shift >= 0; shift -= 8)
    data.push_back(static_cast<uint8_t>(pts >> shift));
}

std::vector<uint8_t> Frame(std::vector<uint8_t> header, size_t size)
{
  header.resize(size, 0);
  return header;
}

// The last two bytes of each frame hold the frame index
void AppendFrames(std::vector<uint8_t>& data, std::vector<uint8_t> frame, int first, int count)
{
  for (int i = first; i < first + count; ++i)
  {
    frame[frame.size() - 2] = static_cast<uint8_t>(i >> 8);
    frame.back() = static_cast<uint8_t>(i);
    data.insert(data.end(), frame.begin(), frame.end());
  }
}

// ADTS header: MPEG-4, no CRC, AAC LC, 44.1 kHz, 2 channels
std::vector<uint8_t> AacFrame()
{
  const unsigned int length = 7 + PAYLOAD_SIZE;
  return Frame({0xFF, 0xF1, 0x50, static_cast<uint8_t>(0x80 | (length >> 11)),
                static_cast<uint8_t>(length >> 3),
                static_cast<uint8_t>(((length & 7) << 5) | 0x1F), 0xFC},
               length);
}

// A segment of frames that start at basePts
std::vector<uint8_t> Segment(const std::vector<uint8_t>& frame, uint64_t basePts, int first = 0)
{
  std::vector<uint8_t> data;
  AppendID3Timestamp(data, basePts);
  AppendFrames(data, frame, first, FRAMES);
  return data;
}

// AC-3, 48 kHz, 192 kb/s (768 byte frames), bsid 8; acmod 2 is stereo, 7 is 3/2
std::vector<uint8_t> Ac3Frame(uint8_t acmodByte)
{
  return Frame({0x0B, 0x77, 0, 0, 0x14, 0x40, acmodByte}, 768);
}

// E-AC-3, 48 kHz, 256 byte frames, bsid 16. The independent substream is 3/2
// channels, so 256 samples per channel never match the blocks of the frame.
std::vector<uint8_t> Eac3Frame(uint8_t numblkscod)
{
  return Frame({0x0B, 0x77, 0x00, 0x7F, static_cast<uint8_t>((numblkscod << 4) | 0x0E), 0x80},
               256);
}

// The dependent substream of 7.1 channels (strmtyp 1), 6 blocks
std::vector<uint8_t> Eac3DependentFrame()
{
  return Frame({0x0B, 0x77, 0x40, 0x7F, 0x34, 0x80}, 256);
}

class ADTSSampleReaderTest : public ::testing::Test
{
protected:
  // A new segment: the session resets the reader and sets the segment start time
  void OpenSegment(const std::vector<uint8_t>& data, uint64_t segmentStart)
  {
    m_reader.reset();
    m_segments = std::make_unique<adaptive::AdaptiveStream>();
    m_segments->m_segments = {data};
    m_segments->m_starts = {segmentStart};
    m_segments->m_onSegmentChanged = [this](uint64_t start) { m_reader->SetPTSOffset(start); };
    m_stream = std::make_unique<CAdaptiveByteStream>(m_segments.get());
    m_reader = std::make_unique<CADTSSampleReader>(m_stream.get(), 1);
    m_reader->Reset(false);
    m_reader->SetPTSOffset(segmentStart);
  }

  // The next segment is downloaded
  void AppendSegment(const std::vector<uint8_t>& data, uint64_t segmentStart)
  {
    m_segments->m_segments.push_back(data);
    m_segments->m_starts.push_back(segmentStart);
  }

  // The frame index in the last two bytes of the sample
  int FrameIndex(size_t end) const
  {
    const AP4_Byte* data{m_reader->GetSampleData()};
    return end < 2 ? -1 : (data[end - 2] << 8) | data[end - 1];
  }
  int FrameIndex() const { return FrameIndex(m_reader->GetSampleDataSize()); }

  std::unique_ptr<adaptive::AdaptiveStream> m_segments;
  std::unique_ptr<CAdaptiveByteStream> m_stream;
  std::unique_ptr<CADTSSampleReader> m_reader;
};
} // namespace

// A seek into a new segment resets the reader. The reader must still advance
// to the target; otherwise audio starts at the segment start, seconds before
// the video.
TEST_F(ADTSSampleReaderTest, SeekIntoNewSegmentReturnsTargetSample)
{
  OpenSegment(Segment(AacFrame(), BASE_PTS), AacTime(0));

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(100), false));
  EXPECT_EQ(m_reader->PTS(), AacTime(100));
  EXPECT_EQ(FrameIndex(), 100);
  EXPECT_FALSE(m_reader->EOS());
}

TEST_F(ADTSSampleReaderTest, SeekBetweenSamplesReturnsNextSample)
{
  OpenSegment(Segment(AacFrame(), BASE_PTS), AacTime(0));

  // One 90 kHz tick after a sample
  const uint64_t target{(BASE_PTS + 100ULL * 1024 * 90000 / 44100 + 1) * 100 / 9 + 1};
  ASSERT_TRUE(m_reader->TimeSeek(target, false));
  EXPECT_EQ(m_reader->PTS(), AacTime(101));
  EXPECT_EQ(FrameIndex(), 101);
}

TEST_F(ADTSSampleReaderTest, SeekToSegmentStartReturnsFirstSample)
{
  OpenSegment(Segment(AacFrame(), BASE_PTS), AacTime(0));

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(0), false));
  EXPECT_EQ(m_reader->PTS(), AacTime(0));
  EXPECT_EQ(FrameIndex(), 0);
}

TEST_F(ADTSSampleReaderTest, SeekToLastSampleSucceeds)
{
  OpenSegment(Segment(AacFrame(), BASE_PTS), AacTime(0));

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(FRAMES - 1), false));
  EXPECT_EQ(m_reader->PTS(), AacTime(FRAMES - 1));
  EXPECT_EQ(FrameIndex(), FRAMES - 1);
  EXPECT_FALSE(m_reader->EOS());
}

TEST_F(ADTSSampleReaderTest, SeekPastStreamEndFails)
{
  OpenSegment(Segment(AacFrame(), BASE_PTS), AacTime(0));

  EXPECT_FALSE(m_reader->TimeSeek(AacTime(FRAMES + 10), false));
  EXPECT_TRUE(m_reader->EOS());
}

// In a live stream, the segment with the target can be not available yet. The
// seek must continue in that segment, not end the audio stream.
TEST_F(ADTSSampleReaderTest, LiveSeekContinuesInTheNextSegment)
{
  OpenSegment(Segment(AacFrame(), BASE_PTS), AacTime(0));
  m_segments->m_waitingForSegment = true;
  const uint64_t nextStart{BASE_PTS + FRAMES * 1024ULL * 90000 / 44100};
  const uint64_t target{FrameTime(nextStart, 50, 1024, 44100)};

  ASSERT_TRUE(m_reader->TimeSeek(target, false));
  EXPECT_FALSE(m_reader->EOS());
  // The session orders the streams by the PTS, and sends no sample while the seek waits
  EXPECT_EQ(m_reader->PTS(), target);
  EXPECT_EQ(m_reader->GetSampleDataSize(), 0u);

  AppendSegment(Segment(AacFrame(), nextStart, FRAMES), nextStart * 100 / 9);
  m_segments->m_waitingForSegment = false;

  ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  EXPECT_EQ(m_reader->PTS(), target);
  EXPECT_EQ(FrameIndex(), FRAMES + 50);
  EXPECT_EQ(m_reader->GetPTSDiff(), 0);
}

// The PTS diff compares the segment start time with the first sample of the
// segment, not with the sample the seek stops at.
TEST_F(ADTSSampleReaderTest, SeekKeepsTheSegmentPtsDiff)
{
  OpenSegment(Segment(AacFrame(), BASE_PTS), AacTime(0) - 5000);

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(100), false));
  EXPECT_EQ(m_reader->GetPTSDiff(), 5000);
}

TEST_F(ADTSSampleReaderTest, SeekForwardInTheSameSegment)
{
  OpenSegment(Segment(AacFrame(), BASE_PTS), AacTime(0));
  ASSERT_TRUE(m_reader->TimeSeek(AacTime(10), false));

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(150), false));
  EXPECT_EQ(m_reader->PTS(), AacTime(150));
  EXPECT_EQ(FrameIndex(), 150);
}

// Without an ID3 timestamp the samples count from zero; the seek must still end.
TEST_F(ADTSSampleReaderTest, SeekWithoutTimestampEnds)
{
  std::vector<uint8_t> data;
  AppendFrames(data, AacFrame(), 0, FRAMES);
  OpenSegment(data, 0);

  ASSERT_TRUE(m_reader->TimeSeek(FrameTime(0, 50, 1024, 44100), false));
  EXPECT_EQ(m_reader->PTS(), FrameTime(0, 50, 1024, 44100));
  EXPECT_EQ(FrameIndex(), 50);
  EXPECT_FALSE(m_reader->TimeSeek(AacTime(0), false));
}

// An AC-3 frame always holds 6 blocks of 256 samples, for any channel count.
TEST_F(ADTSSampleReaderTest, Ac3StereoFramesHold1536Samples)
{
  OpenSegment(Segment(Ac3Frame(0x40), BASE_PTS), AacTime(0));

  ASSERT_TRUE(m_reader->TimeSeek(FrameTime(BASE_PTS, 100, 1536, 48000), false));
  EXPECT_EQ(m_reader->PTS(), FrameTime(BASE_PTS, 100, 1536, 48000));
  EXPECT_EQ(FrameIndex(), 100);
}

TEST_F(ADTSSampleReaderTest, Ac3SurroundFramesHold1536Samples)
{
  OpenSegment(Segment(Ac3Frame(0xE0), BASE_PTS), AacTime(0));

  ASSERT_TRUE(m_reader->TimeSeek(FrameTime(BASE_PTS, 100, 1536, 48000), false));
  EXPECT_EQ(m_reader->PTS(), FrameTime(BASE_PTS, 100, 1536, 48000));
  EXPECT_EQ(FrameIndex(), 100);
}

// An E-AC-3 frame holds 1, 2, 3 or 6 blocks of 256 samples (numblkscod).
TEST_F(ADTSSampleReaderTest, Eac3FramesHoldTheirBlockCount)
{
  const struct
  {
    uint8_t numblkscod;
    uint64_t samples;
  } cases[] = {{0, 256}, {1, 512}, {2, 768}, {3, 1536}};
  for (const auto& c : cases)
  {
    SCOPED_TRACE(static_cast<int>(c.numblkscod));
    OpenSegment(Segment(Eac3Frame(c.numblkscod), BASE_PTS), AacTime(0));

    ASSERT_TRUE(m_reader->TimeSeek(FrameTime(BASE_PTS, 100, c.samples, 48000), false));
    EXPECT_EQ(m_reader->PTS(), FrameTime(BASE_PTS, 100, c.samples, 48000));
    EXPECT_EQ(FrameIndex(), 100);
  }
}

// With 7.1 channels, a dependent substream follows each independent substream.
// Both substreams are one access unit: one sample, one time step.
TEST_F(ADTSSampleReaderTest, Eac3DependentSubstreamIsPartOfTheAccessUnit)
{
  std::vector<uint8_t> data;
  AppendID3Timestamp(data, BASE_PTS);
  for (int i = 0; i < FRAMES; ++i)
  {
    AppendFrames(data, Eac3Frame(3), i, 1);
    AppendFrames(data, Eac3DependentFrame(), i, 1);
  }
  OpenSegment(data, AacTime(0));

  ASSERT_TRUE(m_reader->TimeSeek(FrameTime(BASE_PTS, 100, 1536, 48000), false));
  EXPECT_EQ(m_reader->PTS(), FrameTime(BASE_PTS, 100, 1536, 48000));
  ASSERT_EQ(m_reader->GetSampleDataSize(), 512u);
  EXPECT_EQ(FrameIndex(256), 100);
  EXPECT_EQ(FrameIndex(), 100);

  ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  EXPECT_EQ(m_reader->PTS(), FrameTime(BASE_PTS, 101, 1536, 48000));
  EXPECT_EQ(FrameIndex(), 101);

  // The last access unit of the segment
  ASSERT_TRUE(m_reader->TimeSeek(FrameTime(BASE_PTS, FRAMES - 1, 1536, 48000), false));
  EXPECT_EQ(m_reader->GetSampleDataSize(), 512u);
  EXPECT_EQ(FrameIndex(), FRAMES - 1);
}

// Each segment ends with an independent substream. The search for a dependent
// substream must not move the stream, so the next segment continues the samples.
TEST_F(ADTSSampleReaderTest, Eac3ReadsTheLastFrameOfASegment)
{
  OpenSegment(Segment(Eac3Frame(3), BASE_PTS), AacTime(0));
  const uint64_t nextStart{BASE_PTS + FRAMES * 1536ULL * 90000 / 48000};
  AppendSegment(Segment(Eac3Frame(3), nextStart, FRAMES), nextStart * 100 / 9);

  ASSERT_TRUE(m_reader->TimeSeek(FrameTime(BASE_PTS, FRAMES - 1, 1536, 48000), false));
  EXPECT_EQ(m_reader->PTS(), FrameTime(BASE_PTS, FRAMES - 1, 1536, 48000));
  ASSERT_EQ(m_reader->GetSampleDataSize(), 256u);
  EXPECT_EQ(FrameIndex(), FRAMES - 1);

  ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  EXPECT_EQ(m_reader->PTS(), FrameTime(nextStart, 0, 1536, 48000));
  EXPECT_EQ(FrameIndex(), FRAMES);
}

TEST_F(ADTSSampleReaderTest, Eac3ReadsTheLastAccessUnitOfASegment)
{
  std::vector<uint8_t> segments[2];
  const uint64_t starts[2]{BASE_PTS, BASE_PTS + FRAMES * 1536ULL * 90000 / 48000};
  for (int s = 0; s < 2; ++s)
  {
    AppendID3Timestamp(segments[s], starts[s]);
    for (int i = s * FRAMES; i < (s + 1) * FRAMES; ++i)
    {
      AppendFrames(segments[s], Eac3Frame(3), i, 1);
      AppendFrames(segments[s], Eac3DependentFrame(), i, 1);
    }
  }
  OpenSegment(segments[0], AacTime(0));
  AppendSegment(segments[1], starts[1] * 100 / 9);

  ASSERT_TRUE(m_reader->TimeSeek(FrameTime(BASE_PTS, FRAMES - 1, 1536, 48000), false));
  ASSERT_EQ(m_reader->GetSampleDataSize(), 512u);
  EXPECT_EQ(FrameIndex(), FRAMES - 1);

  ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  EXPECT_EQ(m_reader->PTS(), FrameTime(starts[1], 0, 1536, 48000));
  ASSERT_EQ(m_reader->GetSampleDataSize(), 512u);
  EXPECT_EQ(FrameIndex(256), FRAMES);
  EXPECT_EQ(FrameIndex(), FRAMES);
}

// A segment can end with up to 16 bytes of padding after the last frame, and
// the dependent substream can be shorter than the 4 bytes of the peek plus a header.
TEST_F(ADTSSampleReaderTest, Eac3ReadsThePaddedEndOfASegment)
{
  const uint64_t starts[2]{BASE_PTS, BASE_PTS + FRAMES * 1536ULL * 90000 / 48000};
  for (const bool dependent : {false, true})
  {
    for (size_t padding = 0; padding <= 16; ++padding)
    {
      SCOPED_TRACE(testing::Message() << "dependent " << dependent << ", padding " << padding);
      std::vector<uint8_t> segments[2];
      for (int s = 0; s < 2; ++s)
      {
        AppendID3Timestamp(segments[s], starts[s]);
        for (int i = s * FRAMES; i < (s + 1) * FRAMES; ++i)
        {
          AppendFrames(segments[s], Eac3Frame(3), i, 1);
          if (dependent)
            AppendFrames(segments[s], Frame({0x0B, 0x77, 0x40, 0x07, 0x34, 0x80}, 16), i, 1);
        }
      }
      segments[0].insert(segments[0].end(), padding, 0);
      OpenSegment(segments[0], AacTime(0));
      AppendSegment(segments[1], starts[1] * 100 / 9);

      ASSERT_TRUE(m_reader->TimeSeek(FrameTime(BASE_PTS, FRAMES - 1, 1536, 48000), false));
      EXPECT_EQ(m_reader->GetSampleDataSize(), dependent ? 272u : 256u);
      EXPECT_EQ(FrameIndex(), FRAMES - 1);

      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      EXPECT_EQ(m_reader->PTS(), FrameTime(starts[1], 0, 1536, 48000));
      EXPECT_EQ(FrameIndex(), FRAMES);
    }
  }
}

// The session sets the PTS offset of the next segment when the stream switches
// to that segment. The PTS diff compares the offset with the first sample of
// that segment, and the new diff applies from that sample. Before, the last
// sample of a segment took the offset of the next segment, and the diff was
// one frame wrong until the next seek.
TEST_F(ADTSSampleReaderTest, PtsDiffChangesWithTheFirstFrameOfASegment)
{
  // Each segment has a different diff, so the test sees where the diff changes
  constexpr int64_t DIFF{5000};
  constexpr int64_t NEXT_DIFF{6000};
  const struct
  {
    const char* name;
    std::vector<uint8_t> frame;
    std::vector<uint8_t> dependent;
    uint64_t samples;
    uint64_t rate;
  } codecs[] = {{"AAC", AacFrame(), {}, 1024, 44100},
                {"AC-3", Ac3Frame(0x40), {}, 1536, 48000},
                {"E-AC-3", Eac3Frame(3), {}, 1536, 48000},
                {"E-AC-3 7.1", Eac3Frame(3), Eac3DependentFrame(), 1536, 48000}};
  for (const auto& codec : codecs)
  {
    for (const size_t padding : {0, 1, 3, 15, 16})
    for (const bool late : {false, true})
    {
      SCOPED_TRACE(testing::Message() << codec.name << ", padding " << padding
                                      << (late ? ", next segment after a wait" : ""));
      const uint64_t starts[2]{BASE_PTS,
                               BASE_PTS + FRAMES * codec.samples * 90000 / codec.rate};
      std::vector<uint8_t> segments[2];
      for (int s = 0; s < 2; ++s)
      {
        AppendID3Timestamp(segments[s], starts[s]);
        for (int i = s * FRAMES; i < (s + 1) * FRAMES; ++i)
        {
          AppendFrames(segments[s], codec.frame, i, 1);
          if (!codec.dependent.empty())
            AppendFrames(segments[s], codec.dependent, i, 1);
        }
        segments[s].insert(segments[s].end(), padding, 0);
      }
      OpenSegment(segments[0], starts[0] * 100 / 9 - DIFF);
      if (!late)
        AppendSegment(segments[1], starts[1] * 100 / 9 - NEXT_DIFF);
      m_segments->m_waitingForSegment = late;

      ASSERT_TRUE(
          m_reader->TimeSeek(FrameTime(BASE_PTS, FRAMES - 3, codec.samples, codec.rate), false));
      for (int i = FRAMES - 3; i < FRAMES + 3; ++i)
      {
        SCOPED_TRACE(i);
        if (late && i == FRAMES)
        {
          // The live stream waits for the next segment
          ASSERT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);
          ASSERT_FALSE(m_reader->EOS());
          AppendSegment(segments[1], starts[1] * 100 / 9 - NEXT_DIFF);
          m_segments->m_waitingForSegment = false;
        }
        if (i > FRAMES - 3)
          ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
        EXPECT_EQ(FrameIndex(), i);
        EXPECT_EQ(m_reader->PTS(), FrameTime(BASE_PTS, i, codec.samples, codec.rate));
        EXPECT_EQ(m_reader->GetPTSDiff(), i < FRAMES ? DIFF : NEXT_DIFF);
      }
    }
  }
}
