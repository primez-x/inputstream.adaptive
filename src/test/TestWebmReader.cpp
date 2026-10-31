/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AdaptiveByteStream.h"
#include "TestAdaptiveStream.h"
#include "samplereader/WebmSampleReader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{
constexpr uint64_t BASE_MS = 100000; // 100 s
constexpr uint64_t FRAME_MS = 20;
constexpr int CLUSTERS = 3;
constexpr int CLUSTER_FRAMES = 10;
constexpr int64_t DIFF = 5000;

// PTS of a frame in the microseconds of the sample reader
uint64_t FrameTime(int frame)
{
  return (BASE_MS + frame * FRAME_MS) * 1000;
}

// An EBML element with an 8 byte size
std::vector<uint8_t> Element(std::vector<uint8_t> id, const std::vector<uint8_t>& body)
{
  id.push_back(0x01);
  for (int shift = 48; shift >= 0; shift -= 8)
    id.push_back(static_cast<uint8_t>(body.size() >> shift));
  id.insert(id.end(), body.begin(), body.end());
  return id;
}

std::vector<uint8_t> UInt(std::vector<uint8_t> id, uint64_t value)
{
  std::vector<uint8_t> body;
  for (int shift = 56; shift >= 0; shift -= 8)
    body.push_back(static_cast<uint8_t>(value >> shift));
  return Element(id, body);
}

std::vector<uint8_t> Concat(std::initializer_list<std::vector<uint8_t>> parts)
{
  std::vector<uint8_t> data;
  for (const auto& part : parts)
    data.insert(data.end(), part.begin(), part.end());
  return data;
}

// The initialization segment: EBML header, segment of unknown size, one VP9 track
std::vector<uint8_t> Initialization()
{
  const std::string codec{"V_VP9"};
  const std::vector<uint8_t> track{Concat(
      {UInt({0xD7}, 1), UInt({0x73, 0xC5}, 1), UInt({0x83}, 1),
       Element({0x86}, std::vector<uint8_t>(codec.begin(), codec.end())),
       Element({0xE0}, Concat({UInt({0xB0}, 16), UInt({0xBA}, 16)}))})};
  const std::string docType{"webm"};
  return Concat({Element({0x1A, 0x45, 0xDF, 0xA3},
                         Element({0x42, 0x82}, std::vector<uint8_t>(docType.begin(), docType.end()))),
                 {0x18, 0x53, 0x80, 0x67, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                 Element({0x16, 0x54, 0xAE, 0x6B}, Element({0xAE}, track))});
}

// One cluster per media segment. The last two bytes of each frame hold the frame index.
std::vector<uint8_t> Cluster(int cluster)
{
  std::vector<uint8_t> body{UInt({0xE7}, BASE_MS + cluster * CLUSTER_FRAMES * FRAME_MS)};
  for (int i = 0; i < CLUSTER_FRAMES; ++i)
  {
    const int frame{cluster * CLUSTER_FRAMES + i};
    const uint64_t timecode{i * FRAME_MS};
    std::vector<uint8_t> block{0x81, static_cast<uint8_t>(timecode >> 8),
                               static_cast<uint8_t>(timecode), 0x80};
    block.resize(block.size() + 30, 0);
    block.push_back(static_cast<uint8_t>(frame >> 8));
    block.push_back(static_cast<uint8_t>(frame));
    const std::vector<uint8_t> simpleBlock{Element({0xA3}, block)};
    body.insert(body.end(), simpleBlock.begin(), simpleBlock.end());
  }
  return Element({0x1F, 0x43, 0xB6, 0x75}, body);
}

class WebmSampleReaderTest : public ::testing::Test
{
protected:
  // The session opens the reader on the initialization segment
  void Open()
  {
    m_reader.reset();
    m_segments = std::make_unique<adaptive::AdaptiveStream>();
    m_segments->m_segments = {Initialization()};
    m_segments->m_starts = {0};
    m_segments->m_onSegmentChanged = [this](uint64_t start) { m_reader->SetPTSOffset(start); };
    m_stream = std::make_unique<CAdaptiveByteStream>(m_segments.get());
    m_reader = std::make_unique<CWebmSampleReader>(m_stream.get(), 1);
    ASSERT_TRUE(m_reader->Initialize(nullptr));
  }

  // The next segment is downloaded
  void AppendSegment(const std::vector<uint8_t>& data, uint64_t segmentStart)
  {
    m_segments->m_segments.push_back(data);
    m_segments->m_starts.push_back(segmentStart);
  }

  int FrameIndex() const
  {
    const AP4_Byte* data{m_reader->GetSampleData()};
    const AP4_Size size{m_reader->GetSampleDataSize()};
    return size < 2 ? -1 : (data[size - 2] << 8) | data[size - 1];
  }

  std::unique_ptr<adaptive::AdaptiveStream> m_segments;
  std::unique_ptr<CAdaptiveByteStream> m_stream;
  std::unique_ptr<CWebmSampleReader> m_reader;
};
} // namespace

// The parser reads the header of the next block to find the frame duration.
// After the last frame of a cluster, that read switches to the next segment,
// and the session sets the offset of the next segment. The offset must apply
// to the first frame of the next segment, not to the last frame of the cluster.
TEST_F(WebmSampleReaderTest, PtsDiffStaysTheSameAcrossSegments)
{
  Open();
  for (int c = 0; c < CLUSTERS; ++c)
    AppendSegment(Cluster(c), FrameTime(c * CLUSTER_FRAMES) - DIFF);

  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
  for (int i = 0; i < CLUSTERS * CLUSTER_FRAMES; ++i)
  {
    SCOPED_TRACE(i);
    if (i > 0)
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    EXPECT_EQ(FrameIndex(), i);
    EXPECT_EQ(m_reader->PTS(), FrameTime(i));
    EXPECT_EQ(m_reader->GetPTSDiff(), DIFF);
  }
  EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);
}
