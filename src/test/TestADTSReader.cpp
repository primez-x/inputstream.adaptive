/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "demuxers/ADTSReader.h"
#include "samplereader/ADTSSampleReader.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <bento4/Ap4ByteStream.h>
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

void AppendAacFrames(std::vector<uint8_t>& data, int count)
{
  // ADTS header: MPEG-4, no CRC, AAC LC, 44.1 kHz, 2 channels
  const unsigned int length = 7 + PAYLOAD_SIZE;
  for (int i = 0; i < count; ++i)
  {
    const uint8_t header[] = {0xFF,
                              0xF1,
                              0x50,
                              static_cast<uint8_t>(0x80 | (length >> 11)),
                              static_cast<uint8_t>(length >> 3),
                              static_cast<uint8_t>(((length & 7) << 5) | 0x1F),
                              0xFC};
    data.insert(data.end(), header, header + sizeof(header));
    data.insert(data.end(), PAYLOAD_SIZE, 0);
  }
}

std::vector<uint8_t> AacSegment(bool withTimestamp)
{
  std::vector<uint8_t> data;
  if (withTimestamp)
    AppendID3Timestamp(data, BASE_PTS);
  AppendAacFrames(data, FRAMES);
  return data;
}

class ADTSSampleReaderTest : public ::testing::Test
{
protected:
  // A new segment: the session resets the reader and sets the segment start time
  void OpenSegment(const std::vector<uint8_t>& data, uint64_t segmentStart)
  {
    m_data = data;
    m_stream = new AP4_MemoryByteStream(m_data.data(), static_cast<AP4_Size>(m_data.size()));
    m_reader = std::make_unique<CADTSSampleReader>(m_stream, 1);
    m_reader->Reset(false);
    m_reader->SetPTSOffset(segmentStart);
  }

  void TearDown() override
  {
    m_reader.reset();
    if (m_stream)
      m_stream->Release();
  }

  std::vector<uint8_t> m_data;
  AP4_MemoryByteStream* m_stream{nullptr};
  std::unique_ptr<CADTSSampleReader> m_reader;
};
} // namespace

// A seek into a new segment resets the reader. The reader must still advance
// to the target; otherwise audio starts at the segment start, seconds before
// the video.
TEST_F(ADTSSampleReaderTest, SeekIntoNewSegmentReturnsTargetSample)
{
  OpenSegment(AacSegment(true), AacTime(0));

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(100), false));
  EXPECT_EQ(m_reader->PTS(), AacTime(100));
  EXPECT_FALSE(m_reader->EOS());
}

TEST_F(ADTSSampleReaderTest, SeekBetweenSamplesReturnsNextSample)
{
  OpenSegment(AacSegment(true), AacTime(0));

  // One 90 kHz tick after a sample
  const uint64_t target{(BASE_PTS + 100ULL * 1024 * 90000 / 44100 + 1) * 100 / 9 + 1};
  ASSERT_TRUE(m_reader->TimeSeek(target, false));
  EXPECT_EQ(m_reader->PTS(), AacTime(101));
}

TEST_F(ADTSSampleReaderTest, SeekToSegmentStartReturnsFirstSample)
{
  OpenSegment(AacSegment(true), AacTime(0));

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(0), false));
  EXPECT_EQ(m_reader->PTS(), AacTime(0));
}

TEST_F(ADTSSampleReaderTest, SeekToLastSampleSucceeds)
{
  OpenSegment(AacSegment(true), AacTime(0));

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(FRAMES - 1), false));
  EXPECT_EQ(m_reader->PTS(), AacTime(FRAMES - 1));
  EXPECT_FALSE(m_reader->EOS());
}

TEST_F(ADTSSampleReaderTest, SeekPastSegmentEndFails)
{
  OpenSegment(AacSegment(true), AacTime(0));

  EXPECT_FALSE(m_reader->TimeSeek(AacTime(FRAMES + 10), false));
}

// The PTS diff compares the segment start time with the first sample of the
// segment, not with the sample the seek stops at.
TEST_F(ADTSSampleReaderTest, SeekKeepsTheSegmentPtsDiff)
{
  OpenSegment(AacSegment(true), AacTime(0) - 5000);

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(100), false));
  EXPECT_EQ(m_reader->GetPTSDiff(), 5000);
}

TEST_F(ADTSSampleReaderTest, SeekForwardInTheSameSegment)
{
  OpenSegment(AacSegment(true), AacTime(0));
  ASSERT_TRUE(m_reader->TimeSeek(AacTime(10), false));

  ASSERT_TRUE(m_reader->TimeSeek(AacTime(150), false));
  EXPECT_EQ(m_reader->PTS(), AacTime(150));
}

// Without an ID3 timestamp the samples count from zero; the seek must still end.
TEST_F(ADTSSampleReaderTest, SeekWithoutTimestampEnds)
{
  OpenSegment(AacSegment(false), 0);

  ASSERT_TRUE(m_reader->TimeSeek(FrameTime(0, 50, 1024, 44100), false));
  EXPECT_EQ(m_reader->PTS(), FrameTime(0, 50, 1024, 44100));
  EXPECT_FALSE(m_reader->TimeSeek(AacTime(0), false));
}
