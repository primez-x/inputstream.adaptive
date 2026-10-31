/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AdaptiveByteStream.h"
#include "TestAdaptiveStream.h"
#include "samplereader/TSSampleReader.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

namespace
{
constexpr uint16_t PMT_PID = 0x1000;
constexpr uint16_t VIDEO_PID = 0x100;
constexpr uint16_t AUDIO_PID = 0x101;
constexpr uint32_t VIDEO_ID = 1;
constexpr uint32_t AUDIO_ID = 2;
constexpr uint64_t BASE_PTS = 9000000; // 100 s in 90 kHz ticks
constexpr uint64_t AUDIO_TICKS = 1920; // 1024 samples at 48 kHz
constexpr uint64_t VIDEO_TICKS = 3840;
constexpr int SEGMENTS = 3;
constexpr int SEGMENT_FRAMES = 30; // Audio frames
constexpr int SEGMENT_VIDEO_FRAMES = 15;
// The audio of the muxed segments starts before the video
constexpr uint64_t AUDIO_LEAD = 1000;
// Each segment has a different diff, so the tests see where the diff changes
constexpr int64_t DIFF = 5000;

int64_t SegmentDiff(int segment)
{
  return DIFF + segment * 1000;
}

uint64_t AudioPts(int frame)
{
  return BASE_PTS + frame * AUDIO_TICKS;
}

uint64_t VideoPts(int frame)
{
  return BASE_PTS + frame * VIDEO_TICKS;
}

// PTS in the microseconds of the sample reader
uint64_t Time(uint64_t pts)
{
  return pts * 100 / 9;
}

uint32_t Crc32(const std::vector<uint8_t>& data)
{
  uint32_t crc{0xFFFFFFFF};
  for (const uint8_t byte : data)
  {
    crc ^= static_cast<uint32_t>(byte) << 24;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
  }
  return crc;
}

// ADTS header: MPEG-4, no CRC, AAC LC, 48 kHz, 2 channels. The last two bytes
// of each frame hold the frame index.
std::vector<uint8_t> AacFrame(int index)
{
  constexpr unsigned int length{7 + 120};
  std::vector<uint8_t> frame{0xFF, 0xF1, 0x4C, static_cast<uint8_t>(0x80 | (length >> 11)),
                             static_cast<uint8_t>(length >> 3),
                             static_cast<uint8_t>(((length & 7) << 5) | 0x1F), 0xFC};
  frame.resize(length, 0);
  frame[length - 2] = static_cast<uint8_t>(index >> 8);
  frame[length - 1] = static_cast<uint8_t>(index);
  return frame;
}

// The parser reads an H.264 SPS or PPS only with 64 bytes after the start
// code and a slice header only with 32 bytes. Trailing zero bytes give the
// short access units the size of real ones.
std::vector<uint8_t> PadAccessUnit(std::vector<uint8_t> frame)
{
  frame.resize(frame.size() + 64, 0);
  return frame;
}

// The PMT stream type and the access units of a key frame and of a P-frame
struct VideoCodec
{
  const char* name;
  uint8_t streamType;
  std::vector<uint8_t> keyFrame;
  std::vector<uint8_t> frame;
};

// 16x16 H.264: AUD, SPS, PPS and IDR slice; AUD and P slice
const VideoCodec H264{
    "H.264", 0x1B,
    PadAccessUnit(
        {0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0,
         0x0a, 0xda, 0x7b, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03,
         0x03, 0xc0, 0xf1, 0x22, 0x6a, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8,
         0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0}),
    PadAccessUnit(
        {0x00, 0x00, 0x00, 0x01, 0x09, 0x30, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x20, 0x26, 0x94})};

// 64x64 HEVC: AUD, VPS, SPS, PPS and IDR slice; AUD and P slice
const VideoCodec HEVC{
    "HEVC", 0x24,
    {
     0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x10, 0x00, 0x00, 0x00, 0x01, 0x40, 0x01,
     0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
     0x03, 0x00, 0x00, 0x03, 0x00, 0x1e, 0x92, 0x80, 0x90, 0x00, 0x00, 0x00, 0x01,
     0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03,
     0x00, 0x00, 0x03, 0x00, 0x1e, 0xa0, 0x20, 0x81, 0x05, 0x96, 0x4a, 0x92, 0x4c,
     0xaf, 0x01, 0x68, 0x08, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00,
     0xf0, 0x40, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x72, 0xb4, 0x22, 0x40,
     0x00, 0x00, 0x01, 0x28, 0x01, 0xaf, 0x08, 0x60, 0xf9, 0x2a, 0x5c, 0xff, 0xfb,
     0xcf, 0xe5, 0x62, 0xe8},
    {
     0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x30, 0x00, 0x00, 0x01, 0x02, 0x01, 0xd0,
     0x09, 0x7e, 0x10, 0xc6, 0x10, 0xc0, 0xfb, 0x8c}};

// 16x16 MPEG-2: sequence header, extensions, GOP and I picture; P picture
const VideoCodec MPEG2{
    "MPEG-2", 0x02,
    {
     0x00, 0x00, 0x01, 0xb3, 0x01, 0x00, 0x10, 0x15, 0xff, 0xff, 0xe0, 0x18, 0x00,
     0x00, 0x01, 0xb5, 0x14, 0x8a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0xb8,
     0x00, 0x08, 0x00, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x0f, 0xff, 0xf8, 0x00,
     0x00, 0x01, 0xb5, 0x8f, 0xff, 0xf3, 0x41, 0x80, 0x00, 0x00, 0x01, 0x01, 0x13,
     0xf8, 0x7d, 0x29, 0x48, 0x88},
    {
     0x00, 0x00, 0x01, 0x00, 0x00, 0x57, 0xff, 0xfb, 0x80, 0x00, 0x00, 0x01, 0xb5,
     0x81, 0x1f, 0xf3, 0x41, 0x80, 0x00, 0x00, 0x01, 0x01, 0x12, 0x70}};

// A key frame that uses the parameter sets of an earlier key frame: the access
// unit without the VPS, SPS and PPS, or without the MPEG-2 sequence header and
// sequence extension
std::vector<uint8_t> WithoutParameterSets(const VideoCodec& codec)
{
  const std::vector<uint8_t>& au{codec.keyFrame};
  std::vector<size_t> starts; // The start code of each unit
  for (size_t i = 0; i + 3 < au.size(); ++i)
  {
    if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)
      starts.push_back(i > 0 && au[i - 1] == 0 ? i - 1 : i);
  }
  starts.push_back(au.size());
  std::vector<uint8_t> result;
  for (size_t n = 0; n + 1 < starts.size(); ++n)
  {
    const uint8_t* unit{au.data() + starts[n] + (au[starts[n] + 2] == 1 ? 3 : 4)};
    bool parameterSet;
    if (codec.streamType == H264.streamType)
      parameterSet = (unit[0] & 0x1F) == 7 || (unit[0] & 0x1F) == 8;
    else if (codec.streamType == HEVC.streamType)
      parameterSet = ((unit[0] >> 1) & 0x3F) >= 32 && ((unit[0] >> 1) & 0x3F) <= 34;
    else
      parameterSet = unit[0] == 0xB3 || (unit[0] == 0xB5 && unit[1] >> 4 == 1);
    if (!parameterSet)
      result.insert(result.end(), au.begin() + starts[n], au.begin() + starts[n + 1]);
  }
  return result;
}

// Writes TS packets with continuity counters that continue across segments
class TSWriter
{
public:
  explicit TSWriter(const VideoCodec& codec = H264) : m_codec(codec) {}

  // The PAT and PMT at the start of each HLS segment
  void WriteTables(bool video)
  {
    WriteSection(0, {0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01,
                     static_cast<uint8_t>(0xE0 | (PMT_PID >> 8)), static_cast<uint8_t>(PMT_PID)});
    const uint16_t pcrPid{video ? VIDEO_PID : AUDIO_PID};
    std::vector<uint8_t> pmt{0x02,
                             0xB0,
                             static_cast<uint8_t>(video ? 0x17 : 0x12),
                             0x00,
                             0x01,
                             0xC1,
                             0x00,
                             0x00,
                             static_cast<uint8_t>(0xE0 | (pcrPid >> 8)),
                             static_cast<uint8_t>(pcrPid),
                             0xF0,
                             0x00};
    if (video)
      pmt.insert(pmt.end(), {m_codec.streamType, static_cast<uint8_t>(0xE0 | (VIDEO_PID >> 8)),
                             static_cast<uint8_t>(VIDEO_PID), 0xF0, 0x00});
    pmt.insert(pmt.end(), {0x0F, static_cast<uint8_t>(0xE0 | (AUDIO_PID >> 8)),
                           static_cast<uint8_t>(AUDIO_PID), 0xF0, 0x00});
    WriteSection(PMT_PID, pmt);
  }

  // PES packets of framesPerPes AAC frames. A PES without a length ends only
  // at the start of the next PES.
  void WriteAudio(uint64_t firstPts, int firstFrame, int count, int framesPerPes, bool pesLength)
  {
    for (int frame = firstFrame; frame < firstFrame + count; frame += framesPerPes)
    {
      std::vector<uint8_t> payload;
      for (int i = frame; i < frame + framesPerPes; ++i)
      {
        const std::vector<uint8_t> aac{AacFrame(i)};
        payload.insert(payload.end(), aac.begin(), aac.end());
      }
      WritePes(AUDIO_PID, 0xC0, firstPts + (frame - firstFrame) * AUDIO_TICKS, payload,
               pesLength);
    }
  }

  // One video frame per PES without a length, as HLS segments carry video,
  // with a key frame every keyInterval frames
  void WriteVideo(int firstFrame, int count, int keyInterval = 1)
  {
    for (int frame = firstFrame; frame < firstFrame + count; ++frame)
      WriteVideoFrame(frame, frame % keyInterval ? m_codec.frame : m_codec.keyFrame);
  }

  // firstPacket is the PES size in the first TS packet
  void WriteVideoFrame(int frame, const std::vector<uint8_t>& accessUnit, size_t firstPacket = 184)
  {
    WritePes(VIDEO_PID, 0xE0, VideoPts(frame), accessUnit, false, firstPacket);
  }

  // A frame can span two PES packets, the second without a PTS
  void WriteVideoFrameInTwoPes(int frame, const std::vector<uint8_t>& accessUnit)
  {
    const auto half{accessUnit.begin() + accessUnit.size() / 2};
    WritePes(VIDEO_PID, 0xE0, VideoPts(frame), {accessUnit.begin(), half}, false);
    WritePes(VIDEO_PID, 0xE0, std::nullopt, {half, accessUnit.end()}, false);
  }

  std::vector<uint8_t> Take() { return std::move(m_data); }

private:
  void WritePacket(uint16_t pid, bool unitStart, const uint8_t* payload, size_t size)
  {
    uint8_t packet[188];
    packet[0] = 0x47;
    packet[1] = static_cast<uint8_t>((unitStart ? 0x40 : 0) | (pid >> 8));
    packet[2] = static_cast<uint8_t>(pid);
    uint8_t& cc{m_continuity[pid]};
    if (size < 184)
    {
      // An adaptation field stuffs the rest of the packet
      packet[3] = static_cast<uint8_t>(0x30 | cc);
      packet[4] = static_cast<uint8_t>(183 - size);
      if (size < 183)
      {
        packet[5] = 0;
        std::fill(packet + 6, packet + 188 - size, 0xFF);
      }
    }
    else
      packet[3] = static_cast<uint8_t>(0x10 | cc);
    cc = (cc + 1) & 0x0F;
    std::copy(payload, payload + size, packet + 188 - size);
    m_data.insert(m_data.end(), packet, packet + 188);
  }

  void WriteSection(uint16_t pid, std::vector<uint8_t> section)
  {
    const uint32_t crc{Crc32(section)};
    for (int shift = 24; shift >= 0; shift -= 8)
      section.push_back(static_cast<uint8_t>(crc >> shift));
    section.insert(section.begin(), 0); // pointer field
    WritePacket(pid, true, section.data(), section.size());
  }

  void WritePes(uint16_t pid,
                uint8_t streamId,
                std::optional<uint64_t> pts,
                const std::vector<uint8_t>& payload,
                bool pesLength,
                size_t firstPacket = 184)
  {
    const size_t length{pesLength ? (pts ? 8 : 3) + payload.size() : 0};
    std::vector<uint8_t> pes{0x00,
                             0x00,
                             0x01,
                             streamId,
                             static_cast<uint8_t>(length >> 8),
                             static_cast<uint8_t>(length),
                             0x80,
                             static_cast<uint8_t>(pts ? 0x80 : 0x00),
                             static_cast<uint8_t>(pts ? 0x05 : 0x00)};
    if (pts)
      pes.insert(pes.end(), {static_cast<uint8_t>(0x21 | ((*pts >> 29) & 0x0E)),
                             static_cast<uint8_t>(*pts >> 22),
                             static_cast<uint8_t>(((*pts >> 14) & 0xFE) | 1),
                             static_cast<uint8_t>(*pts >> 7),
                             static_cast<uint8_t>(((*pts << 1) & 0xFE) | 1)});
    pes.insert(pes.end(), payload.begin(), payload.end());
    for (size_t pos = 0, size = firstPacket; pos < pes.size(); pos += size, size = 184)
      WritePacket(pid, pos == 0, pes.data() + pos, std::min<size_t>(size, pes.size() - pos));
  }

  const VideoCodec& m_codec;
  std::vector<uint8_t> m_data;
  std::map<uint16_t, uint8_t> m_continuity;
};

class TSSampleReaderTest : public ::testing::Test
{
protected:
  // The session opens the reader, then sets the offset of the first segment
  void Open(const std::vector<uint8_t>& data, uint64_t segmentStart, INPUTSTREAM_TYPE type)
  {
    m_reader.reset();
    m_segments = std::make_unique<adaptive::AdaptiveStream>();
    m_segments->m_segments = {data};
    m_segments->m_starts = {segmentStart};
    m_segments->m_onSegmentChanged = [this](uint64_t start) { m_reader->SetPTSOffset(start); };
    m_stream = std::make_unique<CAdaptiveByteStream>(m_segments.get());
    m_reader = std::make_unique<CTSSampleReader>(
        m_stream.get(), type, type == INPUTSTREAM_TYPE_VIDEO ? VIDEO_ID : AUDIO_ID, 1U << type);
    ASSERT_TRUE(m_reader->Initialize(nullptr));
    m_reader->SetPTSOffset(segmentStart);
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
  std::unique_ptr<CTSSampleReader> m_reader;
};
} // namespace

// The demuxer emits a PES only when the next PES starts, so after a segment
// change the reader still emits the frames of the previous segment. The offset
// of the new segment must apply from the first frame of that segment, with
// the PTS of the first PES of that segment.
TEST_F(TSSampleReaderTest, PtsDiffChangesWithTheFirstFrameOfASegment)
{
  for (const bool pesLength : {true, false})
  for (const int framesPerPes : {1, 3})
  for (const bool late : {false, true})
  {
    SCOPED_TRACE(testing::Message() << framesPerPes << " frames per PES"
                                    << (pesLength ? "" : ", PES without length")
                                    << (late ? ", next segment after a wait" : ""));
    TSWriter writer;
    std::vector<uint8_t> segments[SEGMENTS];
    uint64_t starts[SEGMENTS];
    for (int s = 0; s < SEGMENTS; ++s)
    {
      writer.WriteTables(false);
      writer.WriteAudio(AudioPts(s * SEGMENT_FRAMES), s * SEGMENT_FRAMES, SEGMENT_FRAMES,
                        framesPerPes, pesLength);
      segments[s] = writer.Take();
      starts[s] = Time(AudioPts(s * SEGMENT_FRAMES)) - SegmentDiff(s);
    }
    Open(segments[0], starts[0], INPUTSTREAM_TYPE_AUDIO);
    int appended{1};
    if (!late)
    {
      for (; appended < SEGMENTS; ++appended)
        AppendSegment(segments[appended], starts[appended]);
    }
    m_segments->m_waitingForSegment = late;

    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    for (int i = 0; i < SEGMENTS * SEGMENT_FRAMES; ++i)
    {
      SCOPED_TRACE(i);
      if (i > 0)
      {
        AP4_Result result{m_reader->ReadSample()};
        if (late && result == AP4_ERROR_EOS && appended < SEGMENTS)
        {
          // The live stream waits for the next segment
          ASSERT_FALSE(m_reader->EOS());
          AppendSegment(segments[appended], starts[appended]);
          m_segments->m_waitingForSegment = ++appended < SEGMENTS;
          result = m_reader->ReadSample();
        }
        ASSERT_EQ(result, AP4_SUCCESS);
      }
      EXPECT_EQ(FrameIndex(), i);
      EXPECT_EQ(m_reader->PTS(), Time(AudioPts(i)));
      EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(i / SEGMENT_FRAMES));
    }
    EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);
    EXPECT_TRUE(m_reader->EOS());
  }
}

// The segments of the video set the offset. The video parser emits a frame
// only after the start of the next frame, so the last video frames of a
// segment come after the PES header of the next segment. The audio of a
// segment can start before the video, and the diff must not take the audio
// start.
TEST_F(TSSampleReaderTest, PtsDiffChangesWithTheVideoOfASegment)
{
  for (const bool withAudio : {false, true})
  for (const int framesPerPes : {1, 3})
  {
    SCOPED_TRACE(testing::Message() << (withAudio ? "video and audio, " : "video, ")
                                    << framesPerPes << " audio frames per PES");
    TSWriter writer;
    std::vector<uint8_t> segments[SEGMENTS];
    uint64_t starts[SEGMENTS];
    for (int s = 0; s < SEGMENTS; ++s)
    {
      writer.WriteTables(true);
      // A muxer interleaves the PES packets by PTS
      int audio{s * SEGMENT_FRAMES};
      int video{s * SEGMENT_VIDEO_FRAMES};
      while (audio < (s + 1) * SEGMENT_FRAMES || video < (s + 1) * SEGMENT_VIDEO_FRAMES)
      {
        if (video == (s + 1) * SEGMENT_VIDEO_FRAMES ||
            (audio < (s + 1) * SEGMENT_FRAMES && AudioPts(audio) - AUDIO_LEAD <= VideoPts(video)))
        {
          writer.WriteAudio(AudioPts(audio) - AUDIO_LEAD, audio, framesPerPes, framesPerPes,
                            false);
          audio += framesPerPes;
        }
        else
          writer.WriteVideo(video++, 1);
      }
      segments[s] = writer.Take();
      starts[s] = Time(VideoPts(s * SEGMENT_VIDEO_FRAMES)) - SegmentDiff(s);
    }
    Open(segments[0], starts[0], INPUTSTREAM_TYPE_VIDEO);
    for (int s = 1; s < SEGMENTS; ++s)
      AppendSegment(segments[s], starts[s]);
    if (withAudio)
      m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);

    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    int video{0};
    int audio{0};
    // Each frame takes the diff of its own segment
    do
    {
      SCOPED_TRACE(testing::Message() << "video frame " << video << ", audio frame " << audio);
      const bool isVideo{m_reader->GetStreamId() == VIDEO_ID};
      const int segment{isVideo ? video / SEGMENT_VIDEO_FRAMES : audio / SEGMENT_FRAMES};
      if (isVideo)
      {
        EXPECT_EQ(m_reader->PTS(), Time(VideoPts(video)));
        ++video;
      }
      else
      {
        EXPECT_EQ(FrameIndex(), audio);
        EXPECT_EQ(m_reader->PTS(), Time(AudioPts(audio) - AUDIO_LEAD));
        ++audio;
      }
      EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(segment));
    } while (AP4_SUCCEEDED(m_reader->ReadSample()));

    EXPECT_EQ(video, SEGMENTS * SEGMENT_VIDEO_FRAMES);
    EXPECT_EQ(audio, withAudio ? SEGMENTS * SEGMENT_FRAMES : 0);
    EXPECT_TRUE(m_reader->EOS());
  }
}

// A seek to the last frame of the stream lands on that frame. A seek past the
// end of the stream fails, so the session ends the stream.
TEST_F(TSSampleReaderTest, SeekPastTheEndFails)
{
  TSWriter writer;
  writer.WriteTables(false);
  writer.WriteAudio(AudioPts(0), 0, SEGMENT_FRAMES, 1, false);
  Open(writer.Take(), Time(AudioPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);

  ASSERT_TRUE(m_reader->TimeSeek(Time(AudioPts(10)), true));
  EXPECT_EQ(FrameIndex(), 10);
  EXPECT_EQ(m_reader->PTS(), Time(AudioPts(10)));
  ASSERT_TRUE(m_reader->TimeSeek(Time(AudioPts(SEGMENT_FRAMES - 1)), true));
  EXPECT_EQ(FrameIndex(), SEGMENT_FRAMES - 1);
  EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);
  EXPECT_FALSE(m_reader->TimeSeek(Time(AudioPts(SEGMENT_FRAMES)), true));
  EXPECT_TRUE(m_reader->EOS());
}

// A seek past the last frame of a live stream waits for the next segment. The
// session aligns the other streams to the PTS of the seek, and the seek
// continues in the next segment.
TEST_F(TSSampleReaderTest, SeekWaitsForTheNextLiveSegment)
{
  TSWriter writer;
  std::vector<uint8_t> segments[2];
  for (int s = 0; s < 2; ++s)
  {
    writer.WriteTables(false);
    writer.WriteAudio(AudioPts(s * SEGMENT_FRAMES), s * SEGMENT_FRAMES, SEGMENT_FRAMES, 1,
                      false);
    segments[s] = writer.Take();
  }
  Open(segments[0], Time(AudioPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
  m_segments->m_waitingForSegment = true;
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);

  const uint64_t target{Time(AudioPts(SEGMENT_FRAMES))};
  ASSERT_TRUE(m_reader->TimeSeek(target, true));
  EXPECT_FALSE(m_reader->EOS());
  EXPECT_EQ(m_reader->PTS(), target);
  EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);
  EXPECT_FALSE(m_reader->EOS());
  EXPECT_EQ(m_reader->PTS(), target);

  AppendSegment(segments[1], Time(AudioPts(SEGMENT_FRAMES)) - SegmentDiff(1));
  m_segments->m_waitingForSegment = false;
  for (int i = SEGMENT_FRAMES; i < SEGMENT_FRAMES + 3; ++i)
  {
    SCOPED_TRACE(i);
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    EXPECT_EQ(FrameIndex(), i);
    EXPECT_EQ(m_reader->PTS(), Time(AudioPts(i)));
    EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(1));
  }
}

// A video seek lands on the key frame before the seek time, or with
// preceeding false on the key frame after the seek time. The demuxer emits a
// video frame only after the start of the next frame, and the seek must start
// to read at the PES of the key frame, not at the PES of a later frame. The
// scan cannot read back into a segment after the next segment starts, so the
// last frames of a segment, also a key frame there, come before the switch.
// A seek time in the next segment lands at the earliest on the first key frame
// of the next segment, as a seek that starts there. A seek time can also be
// between the start times of two frames.
TEST_F(TSSampleReaderTest, VideoSeekLandsOnTheKeyFrame)
{
  for (const VideoCodec* codec : {&H264, &HEVC, &MPEG2})
  for (const int keyInterval : {5, 7})
  {
    SCOPED_TRACE(codec->name);
    TSWriter writer{*codec};
    std::vector<uint8_t> segments[2];
    for (int s = 0; s < 2; ++s)
    {
      writer.WriteTables(true);
      writer.WriteVideo(s * SEGMENT_VIDEO_FRAMES, SEGMENT_VIDEO_FRAMES, keyInterval);
      segments[s] = writer.Take();
    }
    for (const bool preceeding : {true, false})
    for (const uint64_t offset : {uint64_t{0}, VIDEO_TICKS / 2})
    for (int target = 0; target < (preceeding ? 2 : 1) * SEGMENT_VIDEO_FRAMES; ++target)
    {
      SCOPED_TRACE(testing::Message() << "key interval " << keyInterval << ", target frame "
                                      << target << " + " << offset << " ticks"
                                      << (preceeding ? "" : ", not preceeding"));
      Open(segments[0], Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
      AppendSegment(segments[1], Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
      bool started{false};
      ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
      for (int i = 0; i < 3; ++i)
        ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      // AdaptiveStream::seek_time() restarts the segment of the seek time, and
      // the session resets the reader
      m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
      m_segments->m_segmentReadPos = 0;
      m_reader->Reset(false);
      const int after{offset ? target + 1 : target};
      int key{preceeding ? target - target % keyInterval
                         : (after + keyInterval - 1) / keyInterval * keyInterval};
      if (target >= SEGMENT_VIDEO_FRAMES)
        key = std::max(key, (SEGMENT_VIDEO_FRAMES + keyInterval - 1) / keyInterval * keyInterval);
      ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(target) + offset), preceeding));
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(key)));
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(key + 1)));
    }
  }
}

// The parsers keep the parameter sets after a reset, but a decode starts only
// at a key frame. So the read after a reset in a segment starts at the next
// key frame.
TEST_F(TSSampleReaderTest, TheReadAfterAResetStartsAtAKeyFrame)
{
  for (const VideoCodec* codec : {&H264, &HEVC, &MPEG2})
  {
    SCOPED_TRACE(codec->name);
    TSWriter writer{*codec};
    writer.WriteTables(true);
    writer.WriteVideo(0, SEGMENT_VIDEO_FRAMES, 5);
    Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    for (int frame = 1; frame <= 2; ++frame)
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    m_reader->Reset(false);
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(5)));
  }
}

// An IDR or IRAP picture can use the parameter sets of an earlier picture,
// which the decoder keeps after a seek. A seek lands on such a key frame, also
// when the seek starts in a later segment without parameter sets.
TEST_F(TSSampleReaderTest, VideoSeekLandsOnAKeyFrameWithoutParameterSets)
{
  constexpr int KEY_INTERVAL{5};
  for (const VideoCodec* codec : {&H264, &HEVC})
  {
    SCOPED_TRACE(codec->name);
    const std::vector<uint8_t> key{WithoutParameterSets(*codec)};
    ASSERT_LT(key.size(), codec->keyFrame.size());
    TSWriter writer{*codec};
    std::vector<uint8_t> segments[2];
    for (int s = 0; s < 2; ++s)
    {
      writer.WriteTables(true);
      for (int frame = s * SEGMENT_VIDEO_FRAMES; frame < (s + 1) * SEGMENT_VIDEO_FRAMES; ++frame)
      {
        writer.WriteVideoFrame(frame, frame == 0                ? codec->keyFrame
                                      : frame % KEY_INTERVAL ? codec->frame
                                                             : key);
      }
      segments[s] = writer.Take();
    }
    for (int segment = 0; segment < 2; ++segment)
    for (const bool preceeding : {true, false})
    for (int target = segment * SEGMENT_VIDEO_FRAMES;
         target < 2 * SEGMENT_VIDEO_FRAMES - (preceeding ? 0 : KEY_INTERVAL); ++target)
    {
      SCOPED_TRACE(testing::Message() << "segment " << segment << ", target frame " << target
                                      << (preceeding ? "" : ", not preceeding"));
      Open(segments[0], Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
      AppendSegment(segments[1], Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
      bool started{false};
      ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
      while (m_reader->PTS() < Time(VideoPts(segment * SEGMENT_VIDEO_FRAMES + 2)))
        ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      // AdaptiveStream::seek_time() restarts the segment of the seek time, and
      // the session resets the reader
      m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
      m_segments->m_segmentReadPos = 0;
      m_reader->Reset(false);
      const int landing{preceeding ? target - target % KEY_INTERVAL
                                   : (target + KEY_INTERVAL - 1) / KEY_INTERVAL * KEY_INTERVAL};
      ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(target)), preceeding));
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(landing)));
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(landing + 1)));
    }
  }
}

// MPEG-2 has no parameter sets outside the stream, so a decoder can start only
// at a sequence header. A seek lands on an I picture after a sequence header,
// not on an I picture without a sequence header.
TEST_F(TSSampleReaderTest, MPEG2SeekLandsOnAnIPictureAfterASequenceHeader)
{
  const std::vector<uint8_t> iPicture{WithoutParameterSets(MPEG2)};
  ASSERT_LT(iPicture.size(), MPEG2.keyFrame.size());
  TSWriter writer{MPEG2};
  writer.WriteTables(true);
  for (int frame = 0; frame < SEGMENT_VIDEO_FRAMES; ++frame)
  {
    writer.WriteVideoFrame(frame, frame % 10 == 0  ? MPEG2.keyFrame
                                  : frame % 5 == 0 ? iPicture
                                                   : MPEG2.frame);
  }
  const std::vector<uint8_t> segment{writer.Take()};
  for (const bool preceeding : {true, false})
  for (int target = 0; target < (preceeding ? SEGMENT_VIDEO_FRAMES : 10); ++target)
  {
    SCOPED_TRACE(testing::Message() << "target frame " << target
                                    << (preceeding ? "" : ", not preceeding"));
    Open(segment, Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    const int landing{preceeding ? target - target % 10 : (target + 9) / 10 * 10};
    ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(target)), preceeding));
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(landing)));
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(landing + 1)));
  }
}

// After a key frame in decode order, frames with an earlier PTS can follow. A
// seek to the key frame keeps them.
TEST_F(TSSampleReaderTest, VideoSeekKeepsTheFramesAfterTheKeyFrameInDecodeOrder)
{
  constexpr int GOP{3};
  TSWriter writer;
  writer.WriteTables(true);
  // Decode order of each group: the key frame, then the two frames before it
  for (int key = GOP - 1; key < SEGMENT_VIDEO_FRAMES; key += GOP)
  {
    writer.WriteVideo(key, 1);
    writer.WriteVideoFrame(key - 2, H264.frame);
    writer.WriteVideoFrame(key - 1, H264.frame);
  }
  const std::vector<uint8_t> segment{writer.Take()};
  for (int key = 2 * GOP - 1; key < SEGMENT_VIDEO_FRAMES; key += GOP)
  {
    SCOPED_TRACE(testing::Message() << "key frame " << key);
    Open(segment, Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    m_reader->Reset(false);
    ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(key)), true));
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(key)));
    for (const int frame : {key - 2, key - 1})
    {
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(frame)));
    }
  }
}

// A seek with video lands on the PES of the key frame. The session aligns
// the other streams to the PTS of the seek, so the key frame comes first. The
// audio after the PES of the key frame follows with all frames, also frames
// with an earlier PTS. A reset after the seek reads all frames again.
TEST_F(TSSampleReaderTest, SeekReturnsTheVideoFrameFirstAndKeepsTheAudio)
{
  constexpr int KEY{5};
  for (const uint64_t lead : {AUDIO_LEAD, uint64_t{90000}})
  {
    SCOPED_TRACE(testing::Message() << "audio lead " << lead);
    TSWriter writer;
    writer.WriteTables(true);
    for (int v = 0; v < SEGMENT_VIDEO_FRAMES; ++v)
    {
      writer.WriteVideo(v, 1);
      writer.WriteAudio(AudioPts(2 * v) - lead, 2 * v, 2, 2, false);
    }
    Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(KEY)), true));
    ASSERT_EQ(m_reader->GetStreamId(), VIDEO_ID);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(KEY)));
    EXPECT_EQ(m_reader->GetPTSDiff(), DIFF);
    int video{KEY + 1};
    int audio{2 * KEY};
    while (audio < 2 * KEY + 4)
    {
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      if (m_reader->GetStreamId() == VIDEO_ID)
      {
        EXPECT_EQ(m_reader->PTS(), Time(VideoPts(video++)));
        continue;
      }
      EXPECT_EQ(FrameIndex(), audio);
      EXPECT_EQ(m_reader->PTS(), Time(AudioPts(audio++) - lead));
      EXPECT_EQ(m_reader->GetPTSDiff(), DIFF);
    }

    // A reset after a seek drops the audio that waits for the frame of the seek
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(KEY)), true));
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    video = 0;
    audio = 0;
    while (video < 2 || audio < 2)
    {
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      if (m_reader->GetStreamId() == VIDEO_ID)
        EXPECT_EQ(m_reader->PTS(), Time(VideoPts(video++)));
      else
        EXPECT_EQ(FrameIndex(), audio++);
    }
  }
}

// A seek before the first key frame of a segment lands on the segment start.
// A seek after the frames of a segment scans the next segment. With video,
// the video frame of the landing comes first, and the audio after the landing
// follows.
TEST_F(TSSampleReaderTest, SeekToTheSegmentStartReturnsTheVideoFrameFirst)
{
  TSWriter writer;
  std::vector<uint8_t> segments[2];
  for (int s = 0; s < 2; ++s)
  {
    writer.WriteTables(true);
    for (int v = s * SEGMENT_VIDEO_FRAMES; v < (s + 1) * SEGMENT_VIDEO_FRAMES; ++v)
    {
      writer.WriteAudio(AudioPts(2 * v) - AUDIO_LEAD, 2 * v, 2, 1, false);
      writer.WriteVideo(v, 1, 6);
    }
    segments[s] = writer.Take();
  }
  struct Seek
  {
    uint64_t time;
    bool preceeding;
    int frame;
    int audio; // The first audio frame after the landing
  };
  // Key frames 0, 6 and 12, then 18 in the next segment. The audio of a frame
  // comes before the PES of the frame. After a landing on the segment start,
  // the video starts with the first key frame.
  for (const Seek seek : {Seek{VideoPts(0) - AUDIO_LEAD, true, 0, 0},
                          Seek{VideoPts(13), false, 18, 2 * 18 + 2},
                          Seek{VideoPts(16), true, 18, SEGMENT_FRAMES}})
  {
    SCOPED_TRACE(seek.frame);
    Open(segments[0], Time(VideoPts(0)) - SegmentDiff(0), INPUTSTREAM_TYPE_VIDEO);
    AppendSegment(segments[1], Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
    m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    ASSERT_TRUE(m_reader->TimeSeek(Time(seek.time), seek.preceeding));
    ASSERT_EQ(m_reader->GetStreamId(), VIDEO_ID);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(seek.frame)));
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    ASSERT_EQ(m_reader->GetStreamId(), AUDIO_ID);
    EXPECT_EQ(FrameIndex(), seek.audio);
  }
}

// In a live stream, the last frame of a segment comes after the next segment
// starts. A seek to that frame waits with the PTS of the frame, and the frame
// comes before the audio read before it. Each frame takes the diff of its
// segment. A reset while the seek waits reads all frames again.
TEST_F(TSSampleReaderTest, LiveSeekToTheLastFrameOfASegmentReturnsTheFrameFirst)
{
  constexpr int KEY{SEGMENT_VIDEO_FRAMES - 1};
  TSWriter writer;
  std::vector<uint8_t> segments[2];
  for (int s = 0; s < 2; ++s)
  {
    writer.WriteTables(true);
    for (int v = s * SEGMENT_VIDEO_FRAMES; v < (s + 1) * SEGMENT_VIDEO_FRAMES; ++v)
    {
      writer.WriteVideo(v, 1, KEY);
      writer.WriteAudio(AudioPts(2 * v) - AUDIO_LEAD, 2 * v, 2, 1, false);
    }
    segments[s] = writer.Take();
  }
  auto restartSegment = [this]
  {
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
  };
  Open(segments[0], Time(VideoPts(0)) - SegmentDiff(0), INPUTSTREAM_TYPE_VIDEO);
  m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
  m_segments->m_waitingForSegment = true;
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);

  restartSegment();
  ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(KEY)), true));
  restartSegment();
  do
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  while (m_reader->GetStreamId() != VIDEO_ID);
  EXPECT_EQ(m_reader->PTS(), Time(VideoPts(0)));

  restartSegment();
  ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(KEY)), true));
  EXPECT_FALSE(m_reader->EOS());
  EXPECT_EQ(m_reader->PTS(), Time(VideoPts(KEY)));
  EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);

  AppendSegment(segments[1], Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
  m_segments->m_waitingForSegment = false;
  ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  ASSERT_EQ(m_reader->GetStreamId(), VIDEO_ID);
  EXPECT_EQ(m_reader->PTS(), Time(VideoPts(KEY)));
  EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(0));
  int video{KEY + 1};
  int audio{2 * KEY};
  while (audio < SEGMENT_FRAMES + 6)
  {
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    if (m_reader->GetStreamId() == VIDEO_ID)
    {
      SCOPED_TRACE(testing::Message() << "video frame " << video);
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(video)));
      EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(video++ / SEGMENT_VIDEO_FRAMES));
      continue;
    }
    SCOPED_TRACE(testing::Message() << "audio frame " << audio);
    EXPECT_EQ(FrameIndex(), audio);
    EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(audio++ / SEGMENT_FRAMES));
  }
}

// A video seek past the last frame of a live stream continues on the key
// frame before the seek time in the next segment.
TEST_F(TSSampleReaderTest, VideoSeekWaitsForTheNextLiveSegment)
{
  constexpr int KEY_INTERVAL{5};
  TSWriter writer;
  std::vector<uint8_t> segments[2];
  for (int s = 0; s < 2; ++s)
  {
    writer.WriteTables(true);
    writer.WriteVideo(s * SEGMENT_VIDEO_FRAMES, SEGMENT_VIDEO_FRAMES, KEY_INTERVAL);
    segments[s] = writer.Take();
  }
  Open(segments[0], Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
  m_segments->m_waitingForSegment = true;
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);

  const uint64_t target{Time(VideoPts(SEGMENT_VIDEO_FRAMES + 7))};
  ASSERT_TRUE(m_reader->TimeSeek(target, true));
  EXPECT_FALSE(m_reader->EOS());
  EXPECT_EQ(m_reader->PTS(), target);
  EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);

  AppendSegment(segments[1], Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
  m_segments->m_waitingForSegment = false;
  ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  EXPECT_EQ(m_reader->PTS(), Time(VideoPts(SEGMENT_VIDEO_FRAMES + 5)));
  EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(1));
}

// The demuxer emits a PES only when the next PES starts, and the video parser
// emits a frame only when the next frame starts. At the end of the stream,
// the reader reads the last frames of the last PES.
TEST_F(TSSampleReaderTest, TheLastFramesAreReadAtTheEndOfTheStream)
{
  for (const VideoCodec* codec : {&H264, &HEVC, &MPEG2})
  {
    SCOPED_TRACE(codec->name);
    TSWriter writer{*codec};
    writer.WriteTables(true);
    writer.WriteVideo(0, SEGMENT_VIDEO_FRAMES, 5);
    Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    std::vector<uint64_t> expected;
    for (int i = 0; i < SEGMENT_VIDEO_FRAMES; ++i)
      expected.push_back(Time(VideoPts(i)));
    std::vector<uint64_t> read;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
      read.push_back(m_reader->PTS());
    EXPECT_EQ(read, expected);
    EXPECT_TRUE(m_reader->EOS());
  }
}

// A PES without data at the end of the stream makes the parser read the last
// frame before the end of the data. The end still completes the last frame.
TEST_F(TSSampleReaderTest, TheLastFrameBeforeAnEmptyPesIsReadAtTheEndOfTheStream)
{
  for (const VideoCodec* codec : {&H264, &HEVC, &MPEG2})
  {
    SCOPED_TRACE(codec->name);
    TSWriter writer{*codec};
    writer.WriteTables(true);
    writer.WriteVideo(0, SEGMENT_VIDEO_FRAMES, 5);
    writer.WriteVideoFrame(SEGMENT_VIDEO_FRAMES, {});
    Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    std::vector<uint64_t> expected;
    for (int i = 0; i < SEGMENT_VIDEO_FRAMES; ++i)
      expected.push_back(Time(VideoPts(i)));
    std::vector<uint64_t> read;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
      read.push_back(m_reader->PTS());
    EXPECT_EQ(read, expected);
  }
}

// The H.264 parser waits for 32 bytes after a slice start and for 64 bytes
// after an SPS or PPS start. At the end of the stream, no more data follows,
// and a short last frame, as a frame of a fade to black, is read too.
TEST_F(TSSampleReaderTest, AShortLastH264FrameIsReadAtTheEndOfTheStream)
{
  for (const std::vector<uint8_t>* lastFrame : {&H264.keyFrame, &H264.frame})
  {
    SCOPED_TRACE(lastFrame == &H264.keyFrame ? "key frame" : "P-frame");
    TSWriter writer;
    writer.WriteTables(true);
    writer.WriteVideo(0, SEGMENT_VIDEO_FRAMES - 1, 5);
    writer.WriteVideoFrame(SEGMENT_VIDEO_FRAMES - 1,
                           std::vector<uint8_t>(lastFrame->begin(), lastFrame->end() - 64));
    Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    std::vector<uint64_t> read;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
      read.push_back(m_reader->PTS());
    ASSERT_EQ(read.size(), SEGMENT_VIDEO_FRAMES);
    EXPECT_EQ(read.back(), Time(VideoPts(SEGMENT_VIDEO_FRAMES - 1)));
    EXPECT_TRUE(m_reader->EOS());
  }
}

// A live segmenter can end a segment inside a PES. While the reader waits for
// the next segment, the frame of that PES is not complete.
TEST_F(TSSampleReaderTest, AFrameAcrossTwoLiveSegmentsIsReadWhole)
{
  std::vector<uint8_t> bigFrame{H264.frame};
  bigFrame.resize(500, 0);
  constexpr int SPLIT_FRAME{SEGMENT_VIDEO_FRAMES - 1};
  TSWriter writer;
  writer.WriteTables(true);
  writer.WriteVideo(0, SPLIT_FRAME, 5);
  std::vector<uint8_t> first{writer.Take()};
  writer.WriteVideoFrame(SPLIT_FRAME, bigFrame);
  const std::vector<uint8_t> split{writer.Take()};
  first.insert(first.end(), split.begin(), split.begin() + 188);
  writer.WriteTables(true);
  std::vector<uint8_t> second{writer.Take()};
  second.insert(second.end(), split.begin() + 188, split.end());
  writer.WriteVideo(SEGMENT_VIDEO_FRAMES, SEGMENT_VIDEO_FRAMES, 5);
  const std::vector<uint8_t> rest{writer.Take()};
  second.insert(second.end(), rest.begin(), rest.end());

  Open(first, Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
  m_segments->m_waitingForSegment = true;
  std::vector<uint64_t> read;
  AP4_Size splitFrameSize{0};
  bool started{false};
  AP4_Result result{m_reader->Start(started)};
  for (int segment = 0; segment < 2; ++segment)
  {
    for (; AP4_SUCCEEDED(result); result = m_reader->ReadSample())
    {
      read.push_back(m_reader->PTS());
      if (m_reader->PTS() == Time(VideoPts(SPLIT_FRAME)))
        splitFrameSize = m_reader->GetSampleDataSize();
    }
    if (segment == 0)
    {
      EXPECT_FALSE(m_reader->EOS());
      AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
      m_segments->m_waitingForSegment = false;
      result = m_reader->ReadSample();
    }
  }
  std::vector<uint64_t> expected;
  for (int i = 0; i < 2 * SEGMENT_VIDEO_FRAMES; ++i)
    expected.push_back(Time(VideoPts(i)));
  EXPECT_EQ(read, expected);
  EXPECT_GE(splitFrameSize, bigFrame.size() - 1);
  EXPECT_TRUE(m_reader->EOS());
}

// After the seek scan reads the last frames of a segment, the parser again
// waits for the start of the next frame, as a frame can span two PES packets.
TEST_F(TSSampleReaderTest, AFrameInTwoPesPacketsIsReadWholeAfterASeek)
{
  TSWriter writer;
  writer.WriteTables(true);
  writer.WriteVideo(0, SEGMENT_VIDEO_FRAMES, 5);
  const std::vector<uint8_t> first{writer.Take()};
  writer.WriteTables(true);
  for (int frame = SEGMENT_VIDEO_FRAMES; frame < 2 * SEGMENT_VIDEO_FRAMES; ++frame)
    writer.WriteVideoFrameInTwoPes(frame, frame % 5 ? H264.frame : H264.keyFrame);
  Open(first, Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
  AppendSegment(writer.Take(), Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
  m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
  m_segments->m_segmentReadPos = 0;
  m_reader->Reset(false);

  ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(SEGMENT_VIDEO_FRAMES - 1)), true));
  for (int i = 10; i < 2 * SEGMENT_VIDEO_FRAMES; ++i)
  {
    SCOPED_TRACE(i);
    if (i > 10)
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(i)));
    EXPECT_GE(m_reader->GetSampleDataSize(), (i % 5 ? H264.frame : H264.keyFrame).size() - 1);
  }
  EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);
}

// The last audio PES of a segment can come after the first video PES of the
// next segment. Each frame takes the diff of its own segment.
TEST_F(TSSampleReaderTest, PtsDiffOfLateAudioIsTheDiffOfItsSegment)
{
  TSWriter writer;
  for (int s = 0; s < SEGMENTS; ++s)
  {
    writer.WriteTables(true);
    // The segment starts with three video frames and ends with three audio PES
    writer.WriteVideo(s * SEGMENT_VIDEO_FRAMES, 3);
    for (int v = 3; v < SEGMENT_VIDEO_FRAMES; ++v)
    {
      const int audio{s * SEGMENT_FRAMES + 2 * (v - 3)};
      writer.WriteAudio(AudioPts(audio), audio, 2, 2, false);
      writer.WriteVideo(s * SEGMENT_VIDEO_FRAMES + v, 1);
    }
    writer.WriteAudio(AudioPts(s * SEGMENT_FRAMES + 24), s * SEGMENT_FRAMES + 24, 6, 2, false);
    const uint64_t start{Time(VideoPts(s * SEGMENT_VIDEO_FRAMES)) - SegmentDiff(s)};
    if (s == 0)
      Open(writer.Take(), start, INPUTSTREAM_TYPE_VIDEO);
    else
      AppendSegment(writer.Take(), start);
  }
  m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
  int video{0};
  int audio{0};
  bool started{false};
  for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
       result = m_reader->ReadSample())
  {
    if (m_reader->GetStreamId() == VIDEO_ID)
    {
      SCOPED_TRACE(testing::Message() << "video frame " << video);
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(video)));
      EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(video++ / SEGMENT_VIDEO_FRAMES));
    }
    else
    {
      SCOPED_TRACE(testing::Message() << "audio frame " << audio);
      EXPECT_EQ(FrameIndex(), audio);
      EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(audio++ / SEGMENT_FRAMES));
    }
  }
  EXPECT_EQ(video, SEGMENTS * SEGMENT_VIDEO_FRAMES);
  EXPECT_EQ(audio, SEGMENTS * SEGMENT_FRAMES);
}

// The demuxer emits a video frame two PES packets after the PES of the frame.
// With one frame per segment, the segment changes twice before the frame is a
// sample, and the frame still takes the diff of its own segment. The reader
// reads the stream information from the first frame of the first segment.
TEST_F(TSSampleReaderTest, PtsDiffOfSegmentsWithOneFrame)
{
  constexpr int FIRST_FRAMES{3};
  constexpr int FRAMES{8};
  auto segmentOf = [](int frame) { return std::max(0, frame - FIRST_FRAMES + 1); };
  TSWriter writer;
  writer.WriteTables(true);
  writer.WriteVideo(0, FIRST_FRAMES);
  Open(writer.Take(), Time(VideoPts(0)) - SegmentDiff(0), INPUTSTREAM_TYPE_VIDEO);
  for (int frame = FIRST_FRAMES; frame < FRAMES; ++frame)
  {
    writer.WriteTables(true);
    writer.WriteVideo(frame, 1);
    AppendSegment(writer.Take(), Time(VideoPts(frame)) - SegmentDiff(segmentOf(frame)));
  }
  int frame{0};
  bool started{false};
  for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
       result = m_reader->ReadSample(), ++frame)
  {
    SCOPED_TRACE(frame);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(frame)));
    EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(segmentOf(frame)));
  }
  EXPECT_EQ(frame, FRAMES);
}

// An audio frame of a segment can be a sample before the first video PES of
// the segment is read. HLS starts a new period at a discontinuity, so the
// segments of a reader are continuous, and the diff of the segment before
// applies to that frame. The read of the stream information reads the video
// PES of the first segment.
TEST_F(TSSampleReaderTest, PtsDiffOfAudioBeforeTheVideoOfASegment)
{
  constexpr int EARLY_FRAMES{3};
  TSWriter writer;
  for (int s = 0; s < SEGMENTS; ++s)
  {
    writer.WriteTables(true);
    // The segment starts with three audio PES before the first video PES
    const int audio{s * SEGMENT_FRAMES};
    writer.WriteAudio(AudioPts(audio), audio, EARLY_FRAMES, 1, false);
    writer.WriteVideo(s * SEGMENT_VIDEO_FRAMES, SEGMENT_VIDEO_FRAMES);
    writer.WriteAudio(AudioPts(audio + EARLY_FRAMES), audio + EARLY_FRAMES,
                      SEGMENT_FRAMES - EARLY_FRAMES, 3, false);
    const uint64_t start{Time(VideoPts(s * SEGMENT_VIDEO_FRAMES)) - SegmentDiff(s)};
    if (s == 0)
      Open(writer.Take(), start, INPUTSTREAM_TYPE_VIDEO);
    else
      AppendSegment(writer.Take(), start);
  }
  m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
  int video{0};
  int audio{0};
  bool started{false};
  for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
       result = m_reader->ReadSample())
  {
    if (m_reader->GetStreamId() == VIDEO_ID)
    {
      SCOPED_TRACE(testing::Message() << "video frame " << video);
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(video)));
      EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(video++ / SEGMENT_VIDEO_FRAMES));
    }
    else
    {
      SCOPED_TRACE(testing::Message() << "audio frame " << audio);
      EXPECT_EQ(FrameIndex(), audio);
      // The demuxer emits an audio frame when the next audio PES starts
      const int segment{audio / SEGMENT_FRAMES};
      const bool early{segment > 0 && audio % SEGMENT_FRAMES < EARLY_FRAMES - 1};
      EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(early ? segment - 1 : segment));
      ++audio;
    }
  }
  EXPECT_EQ(video, SEGMENTS * SEGMENT_VIDEO_FRAMES);
  EXPECT_EQ(audio, SEGMENTS * SEGMENT_FRAMES);
}

// The header of a PES can start in one segment and end in the next segment.
// The PES and its PTS belong to the segment where the PES starts.
TEST_F(TSSampleReaderTest, PtsDiffOfAPesHeaderAcrossTwoSegments)
{
  TSWriter writer;
  writer.WriteTables(true);
  writer.WriteVideo(0, SEGMENT_VIDEO_FRAMES - 1);
  std::vector<uint8_t> first{writer.Take()};
  // The first TS packet holds 9 of the 14 bytes of the PES header
  writer.WriteVideoFrame(SEGMENT_VIDEO_FRAMES - 1, H264.keyFrame, 9);
  const std::vector<uint8_t> split{writer.Take()};
  first.insert(first.end(), split.begin(), split.begin() + 188);
  writer.WriteTables(true);
  std::vector<uint8_t> second{writer.Take()};
  second.insert(second.end(), split.begin() + 188, split.end());
  writer.WriteVideo(SEGMENT_VIDEO_FRAMES, SEGMENT_VIDEO_FRAMES);
  const std::vector<uint8_t> rest{writer.Take()};
  second.insert(second.end(), rest.begin(), rest.end());
  Open(first, Time(VideoPts(0)) - SegmentDiff(0), INPUTSTREAM_TYPE_VIDEO);
  AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
  int frame{0};
  bool started{false};
  for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
       result = m_reader->ReadSample(), ++frame)
  {
    SCOPED_TRACE(frame);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(frame)));
    EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(frame / SEGMENT_VIDEO_FRAMES));
  }
  EXPECT_EQ(frame, 2 * SEGMENT_VIDEO_FRAMES);
}

// The last key frame of a segment can continue in the next segment, with a
// part of the PES header or with a part of the frame data. The stream cannot
// read the previous segment again, but a seek still lands on that key frame,
// also when the seek waits for the next live segment.
TEST_F(TSSampleReaderTest, SeekLandsOnAKeyFrameAcrossTwoSegments)
{
  constexpr int SPLIT_FRAME{SEGMENT_VIDEO_FRAMES - 1};
  std::vector<uint8_t> bigKeyFrame{H264.keyFrame};
  bigKeyFrame.resize(500, 0);
  // 9 of the 14 bytes of the PES header, or the PES header and a part of the frame
  for (const size_t firstPacket : {size_t{9}, size_t{184}})
  {
    TSWriter writer;
    writer.WriteTables(true);
    writer.WriteVideo(0, SPLIT_FRAME, 5);
    std::vector<uint8_t> first{writer.Take()};
    writer.WriteVideoFrame(SPLIT_FRAME, bigKeyFrame, firstPacket);
    const std::vector<uint8_t> split{writer.Take()};
    first.insert(first.end(), split.begin(), split.begin() + 188);
    writer.WriteTables(true);
    std::vector<uint8_t> second{writer.Take()};
    second.insert(second.end(), split.begin() + 188, split.end());
    writer.WriteVideo(SEGMENT_VIDEO_FRAMES, SEGMENT_VIDEO_FRAMES, 5);
    const std::vector<uint8_t> rest{writer.Take()};
    second.insert(second.end(), rest.begin(), rest.end());

    for (const bool live : {false, true})
    for (const bool preceeding : {true, false})
    for (const uint64_t target : {VideoPts(SPLIT_FRAME - 2), VideoPts(SPLIT_FRAME),
                                  VideoPts(SPLIT_FRAME) + VIDEO_TICKS / 2})
    {
      SCOPED_TRACE(testing::Message() << "first packet " << firstPacket << ", target "
                                      << target << (preceeding ? "" : ", not preceeding")
                                      << (live ? ", live" : ""));
      const int landing{preceeding && target < VideoPts(SPLIT_FRAME) ? 10
                        : !preceeding && target > VideoPts(SPLIT_FRAME) ? SEGMENT_VIDEO_FRAMES
                                                                         : SPLIT_FRAME};
      // The live seeks read the next segment for the landing frame
      if (live && landing < SPLIT_FRAME)
        continue;
      Open(first, Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
      if (live)
        m_segments->m_waitingForSegment = true;
      else
        AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
      // The session seeks to a resume time before the first read
      m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
      m_segments->m_segmentReadPos = 0;
      m_reader->Reset(false);
      ASSERT_TRUE(m_reader->TimeSeek(Time(target), preceeding));
      if (live)
      {
        EXPECT_FALSE(m_reader->EOS());
        AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
        m_segments->m_waitingForSegment = false;
        ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      }
      // The PTS and the PTS diff of each frame
      std::vector<std::pair<uint64_t, int64_t>> read;
      AP4_Size landingSize{m_reader->GetSampleDataSize()};
      do
        read.emplace_back(m_reader->PTS(), m_reader->GetPTSDiff());
      while (AP4_SUCCEEDED(m_reader->ReadSample()));
      std::vector<std::pair<uint64_t, int64_t>> expected;
      for (int frame = landing; frame < 2 * SEGMENT_VIDEO_FRAMES; ++frame)
        expected.emplace_back(Time(VideoPts(frame)), SegmentDiff(frame / SEGMENT_VIDEO_FRAMES));
      EXPECT_EQ(read, expected);
      if (landing == SPLIT_FRAME)
        EXPECT_GE(landingSize, bigKeyFrame.size() - 1);
    }
  }
}

// The last video and audio PES of a segment can continue in the next segment.
// After a seek to the key frame of that video PES, the audio of that audio PES
// follows the key frame, as the stream cannot read the PES again.
TEST_F(TSSampleReaderTest, SeekToAKeyFrameAcrossTwoSegmentsKeepsTheAudioAcrossTheSegments)
{
  constexpr int SPLIT_FRAME{SEGMENT_VIDEO_FRAMES - 1};
  constexpr int SPLIT_AUDIO{2 * SEGMENT_VIDEO_FRAMES};
  constexpr int SPLIT_FRAMES{4};
  // The audio of the next segment comes before or after the video of a frame.
  // With the video first, the rest of the audio PES comes after the next video PES.
  for (const bool audioFirst : {true, false})
  for (const bool preceeding : {true, false})
  {
    SCOPED_TRACE(testing::Message() << (audioFirst ? "audio first" : "video first")
                                    << (preceeding ? "" : ", not preceeding"));
    TSWriter writer;
    writer.WriteTables(true);
    for (int v = 0; v < SPLIT_FRAME; ++v)
    {
      writer.WriteAudio(AudioPts(2 * v) - AUDIO_LEAD, 2 * v, 2, 1, false);
      writer.WriteVideo(v, 1, 5);
    }
    writer.WriteAudio(AudioPts(2 * SPLIT_FRAME) - AUDIO_LEAD, 2 * SPLIT_FRAME, 2, 1, false);
    std::vector<uint8_t> first{writer.Take()};
    // 9 of the 14 bytes of the PES header of the key frame
    writer.WriteVideoFrame(SPLIT_FRAME, H264.keyFrame, 9);
    const std::vector<uint8_t> keyFrame{writer.Take()};
    writer.WriteAudio(AudioPts(SPLIT_AUDIO) - AUDIO_LEAD, SPLIT_AUDIO, SPLIT_FRAMES, SPLIT_FRAMES,
                      false);
    const std::vector<uint8_t> audio{writer.Take()};
    first.insert(first.end(), keyFrame.begin(), keyFrame.begin() + 188);
    first.insert(first.end(), audio.begin(), audio.begin() + 188);
    writer.WriteTables(true);
    std::vector<uint8_t> second{writer.Take()};
    second.insert(second.end(), keyFrame.begin() + 188, keyFrame.end());
    if (audioFirst)
      second.insert(second.end(), audio.begin() + 188, audio.end());
    for (int v = SEGMENT_VIDEO_FRAMES; v < 2 * SEGMENT_VIDEO_FRAMES; ++v)
    {
      const int a{2 * v + SPLIT_FRAMES};
      if (!audioFirst)
        writer.WriteVideo(v, 1, 5);
      if (!audioFirst && v == SEGMENT_VIDEO_FRAMES)
      {
        const std::vector<uint8_t> video{writer.Take()};
        second.insert(second.end(), video.begin(), video.end());
        second.insert(second.end(), audio.begin() + 188, audio.end());
      }
      writer.WriteAudio(AudioPts(a) - AUDIO_LEAD, a, 2, 1, false);
      if (audioFirst)
        writer.WriteVideo(v, 1, 5);
    }
    const std::vector<uint8_t> rest{writer.Take()};
    second.insert(second.end(), rest.begin(), rest.end());

    Open(first, Time(VideoPts(0)) - SegmentDiff(0), INPUTSTREAM_TYPE_VIDEO);
    AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
    m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(SPLIT_FRAME) - (preceeding ? 0 : VIDEO_TICKS / 2)),
                                   preceeding));
    ASSERT_EQ(m_reader->GetStreamId(), VIDEO_ID);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(SPLIT_FRAME)));
    EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(0));
    // The audio frames, and the PTS diff of each
    std::vector<std::pair<int, int64_t>> read;
    int video{SPLIT_FRAME + 1};
    while (read.size() < 2 * SPLIT_FRAMES + 2 && AP4_SUCCEEDED(m_reader->ReadSample()))
    {
      if (m_reader->GetStreamId() == VIDEO_ID)
        EXPECT_EQ(m_reader->PTS(), Time(VideoPts(video++)));
      else
        read.emplace_back(FrameIndex(), m_reader->GetPTSDiff());
    }
    // The seek read the first video PES of the next segment, so all audio of
    // that segment takes the diff of that segment
    std::vector<std::pair<int, int64_t>> expected;
    for (int a = SPLIT_AUDIO; a < SPLIT_AUDIO + 2 * SPLIT_FRAMES + 2; ++a)
      expected.emplace_back(a, SegmentDiff(a < SPLIT_AUDIO + SPLIT_FRAMES ? 0 : 1));
    EXPECT_EQ(read, expected);
  }
}

// The audio can end with a PES that continues in the next segment. After a
// seek to the key frame of the segment end, the reader takes the rest of that
// audio PES and the read continues in the next segments.
TEST_F(TSSampleReaderTest, SeekToAKeyFrameAcrossTwoSegmentsKeepsTheAudioThatEnds)
{
  constexpr int SPLIT_FRAME{SEGMENT_VIDEO_FRAMES - 1};
  constexpr int SPLIT_AUDIO{2 * SEGMENT_VIDEO_FRAMES};
  constexpr int SPLIT_FRAMES{4};
  TSWriter writer;
  writer.WriteTables(true);
  for (int v = 0; v < SPLIT_FRAME; ++v)
  {
    writer.WriteAudio(AudioPts(2 * v) - AUDIO_LEAD, 2 * v, 2, 1, false);
    writer.WriteVideo(v, 1, 5);
  }
  writer.WriteAudio(AudioPts(2 * SPLIT_FRAME) - AUDIO_LEAD, 2 * SPLIT_FRAME, 2, 1, false);
  std::vector<uint8_t> first{writer.Take()};
  writer.WriteVideoFrame(SPLIT_FRAME, H264.keyFrame, 9);
  const std::vector<uint8_t> keyFrame{writer.Take()};
  writer.WriteAudio(AudioPts(SPLIT_AUDIO) - AUDIO_LEAD, SPLIT_AUDIO, SPLIT_FRAMES, SPLIT_FRAMES,
                    false);
  const std::vector<uint8_t> audio{writer.Take()};
  first.insert(first.end(), keyFrame.begin(), keyFrame.begin() + 188);
  first.insert(first.end(), audio.begin(), audio.begin() + 188);
  writer.WriteTables(true);
  std::vector<uint8_t> second{writer.Take()};
  second.insert(second.end(), keyFrame.begin() + 188, keyFrame.end());
  second.insert(second.end(), audio.begin() + 188, audio.end());
  writer.WriteVideo(SEGMENT_VIDEO_FRAMES, SEGMENT_VIDEO_FRAMES, 5);
  const std::vector<uint8_t> rest{writer.Take()};
  second.insert(second.end(), rest.begin(), rest.end());
  writer.WriteTables(true);
  writer.WriteVideo(2 * SEGMENT_VIDEO_FRAMES, SEGMENT_VIDEO_FRAMES, 5);
  const std::vector<uint8_t> third{writer.Take()};

  Open(first, Time(VideoPts(0)) - SegmentDiff(0), INPUTSTREAM_TYPE_VIDEO);
  AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
  AppendSegment(third, Time(VideoPts(2 * SEGMENT_VIDEO_FRAMES)) - SegmentDiff(2));
  m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
  m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
  m_segments->m_segmentReadPos = 0;
  m_reader->Reset(false);
  ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(SPLIT_FRAME)), true));
  std::vector<uint64_t> video;
  std::vector<int> audioFrames;
  do
  {
    if (m_reader->GetStreamId() == VIDEO_ID)
      video.push_back(m_reader->PTS());
    else
      audioFrames.push_back(FrameIndex());
  } while (AP4_SUCCEEDED(m_reader->ReadSample()));
  std::vector<uint64_t> expectedVideo;
  for (int v = SPLIT_FRAME; v < 3 * SEGMENT_VIDEO_FRAMES; ++v)
    expectedVideo.push_back(Time(VideoPts(v)));
  EXPECT_EQ(video, expectedVideo);
  EXPECT_EQ(audioFrames, (std::vector<int>{SPLIT_AUDIO, SPLIT_AUDIO + 1, SPLIT_AUDIO + 2,
                                           SPLIT_AUDIO + 3}));
}

// A PES of audio holds several frames. A seek lands on the frame of the seek
// time, not on the first frame of its PES.
TEST_F(TSSampleReaderTest, AudioSeekLandsOnTheFrameInsideThePes)
{
  TSWriter writer;
  writer.WriteTables(false);
  writer.WriteAudio(AudioPts(0), 0, SEGMENT_FRAMES, 3, false);
  const std::vector<uint8_t> segment{writer.Take()};
  for (const bool preceeding : {true, false})
  for (const uint64_t offset : {uint64_t{0}, AUDIO_TICKS / 2})
  for (int target = 0; target < SEGMENT_FRAMES - 1; ++target)
  {
    SCOPED_TRACE(testing::Message() << "target frame " << target << " + " << offset << " ticks"
                                    << (preceeding ? "" : ", not preceeding"));
    Open(segment, Time(AudioPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    const int frame{preceeding || !offset ? target : target + 1};
    ASSERT_TRUE(m_reader->TimeSeek(Time(AudioPts(target) + offset), preceeding));
    EXPECT_EQ(FrameIndex(), frame);
    EXPECT_EQ(m_reader->PTS(), Time(AudioPts(frame)));
    if (frame + 1 < SEGMENT_FRAMES)
    {
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      EXPECT_EQ(FrameIndex(), frame + 1);
    }
    else
      EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);
  }
}
