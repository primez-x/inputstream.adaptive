/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AdaptiveByteStream.h"
#include "samplereader/ADTSSampleReader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

// The ADTS tests build without common/AdaptiveStream.cpp. This stream serves
// the downloaded segments from memory and can wait for the next live segment.
namespace adaptive
{
class AdaptiveStream
{
public:
  std::vector<uint8_t> m_data;
  size_t m_pos{0};
  bool m_waitingForSegment{false};
};
} // namespace adaptive

AP4_Result CAdaptiveByteStream::ReadPartial(void* buffer, AP4_Size bytesToRead, AP4_Size& bytesRead)
{
  bytesRead = static_cast<AP4_Size>(
      std::min<size_t>(bytesToRead, m_adStream->m_data.size() - m_adStream->m_pos));
  std::memcpy(buffer, m_adStream->m_data.data() + m_adStream->m_pos, bytesRead);
  m_adStream->m_pos += bytesRead;
  return bytesRead > 0 ? AP4_SUCCESS : AP4_ERROR_READ_FAILED;
}

AP4_Result CAdaptiveByteStream::WritePartial(const void* buffer,
                                            AP4_Size bytesToWrite,
                                            AP4_Size& bytesWritten)
{
  return AP4_ERROR_NOT_SUPPORTED;
}

AP4_Result CAdaptiveByteStream::Seek(AP4_Position position)
{
  // AdaptiveStream::seek stays in the downloaded data and stops at the end
  m_adStream->m_pos = std::min<size_t>(position, m_adStream->m_data.size());
  return position <= m_adStream->m_data.size() ? AP4_SUCCESS : AP4_ERROR_NOT_SUPPORTED;
}

AP4_Result CAdaptiveByteStream::Tell(AP4_Position& position)
{
  position = m_adStream->m_pos;
  return AP4_SUCCESS;
}

AP4_Result CAdaptiveByteStream::GetSize(AP4_LargeSize& size)
{
  return AP4_ERROR_NOT_SUPPORTED;
}

bool CAdaptiveByteStream::waitingForSegment() const
{
  return m_adStream->m_waitingForSegment;
}

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

class ADTSSampleReaderTest : public ::testing::Test
{
protected:
  // A new segment: the session resets the reader and sets the segment start time
  void OpenSegment(const std::vector<uint8_t>& data, uint64_t segmentStart)
  {
    m_reader.reset();
    m_segments = std::make_unique<adaptive::AdaptiveStream>();
    m_segments->m_data = data;
    m_stream = std::make_unique<CAdaptiveByteStream>(m_segments.get());
    m_reader = std::make_unique<CADTSSampleReader>(m_stream.get(), 1);
    m_reader->Reset(false);
    m_reader->SetPTSOffset(segmentStart);
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

  const std::vector<uint8_t> next{Segment(AacFrame(), nextStart, FRAMES)};
  m_segments->m_data.insert(m_segments->m_data.end(), next.begin(), next.end());
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

