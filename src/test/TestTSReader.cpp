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

// A 16x16 H.264 IDR access unit (AUD, SPS, PPS and slice) and a P-frame
// access unit (AUD and slice). The parser reads an SPS or a PPS only with 64
// bytes after the start code and a slice header only with 32 bytes. Trailing
// zero bytes give the short access units the size of real ones.
std::vector<uint8_t> PadAccessUnit(std::vector<uint8_t> frame)
{
  frame.resize(frame.size() + 64, 0);
  return frame;
}

const std::vector<uint8_t> H264_IDR_FRAME{PadAccessUnit(
    {0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0,
     0x0a, 0xda, 0x7b, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03,
     0x03, 0xc0, 0xf1, 0x22, 0x6a, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8,
     0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0})};
const std::vector<uint8_t> H264_P_FRAME{PadAccessUnit(
    {0x00, 0x00, 0x00, 0x01, 0x09, 0x30, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x20, 0x26, 0x94})};

// Writes TS packets with continuity counters that continue across segments
class TSWriter
{
public:
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
      pmt.insert(pmt.end(), {0x1B, static_cast<uint8_t>(0xE0 | (VIDEO_PID >> 8)),
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
      WritePes(VIDEO_PID, 0xE0, VideoPts(frame),
               frame % keyInterval ? H264_P_FRAME : H264_IDR_FRAME, false);
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
                uint64_t pts,
                const std::vector<uint8_t>& payload,
                bool pesLength)
  {
    const size_t length{pesLength ? 8 + payload.size() : 0};
    std::vector<uint8_t> pes{0x00,
                             0x00,
                             0x01,
                             streamId,
                             static_cast<uint8_t>(length >> 8),
                             static_cast<uint8_t>(length),
                             0x80,
                             0x80,
                             0x05,
                             static_cast<uint8_t>(0x21 | ((pts >> 29) & 0x0E)),
                             static_cast<uint8_t>(pts >> 22),
                             static_cast<uint8_t>(((pts >> 14) & 0xFE) | 1),
                             static_cast<uint8_t>(pts >> 7),
                             static_cast<uint8_t>(((pts << 1) & 0xFE) | 1)};
    pes.insert(pes.end(), payload.begin(), payload.end());
    for (size_t pos = 0; pos < pes.size(); pos += 184)
      WritePacket(pid, pos == 0, pes.data() + pos, std::min<size_t>(184, pes.size() - pos));
  }

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
    // The last PES of the stream has no next PES
    for (int i = 0; i < SEGMENTS * SEGMENT_FRAMES - framesPerPes; ++i)
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
    // One diff serves both streams. The diff of a segment applies from the
    // first frame of that segment after the last video frame of the previous
    // segment.
    int segment{0};
    do
    {
      SCOPED_TRACE(testing::Message() << "video frame " << video << ", audio frame " << audio);
      const bool isVideo{m_reader->GetStreamId() == VIDEO_ID};
      const int frameSegment{isVideo ? video / SEGMENT_VIDEO_FRAMES : audio / SEGMENT_FRAMES};
      if (frameSegment > segment && video >= frameSegment * SEGMENT_VIDEO_FRAMES)
        segment = frameSegment;
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

    // The last PES of each stream has no next PES. The video parser also waits
    // for the start of the next frame.
    EXPECT_EQ(video, SEGMENTS * SEGMENT_VIDEO_FRAMES - 2);
    EXPECT_EQ(audio, withAudio ? SEGMENTS * SEGMENT_FRAMES - framesPerPes : 0);
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

  const uint64_t target{Time(AudioPts(SEGMENT_FRAMES)) - 1};
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
// to read at the PES of the key frame, not at the PES of a later frame.
TEST_F(TSSampleReaderTest, VideoSeekLandsOnTheKeyFrame)
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
  for (const bool preceeding : {true, false})
  // The scan for a target in the last two frames reads into the next segment
  for (int target = 0; target < SEGMENT_VIDEO_FRAMES - 2; ++target)
  {
    SCOPED_TRACE(testing::Message() << "target frame " << target
                                    << (preceeding ? "" : ", not preceeding"));
    Open(segments[0], Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    AppendSegment(segments[1], Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - DIFF);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    for (int i = 0; i < 3; ++i)
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    // AdaptiveStream::seek_time() restarts the segment of the seek time, and
    // the session resets the reader
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    const int key{preceeding ? target - target % KEY_INTERVAL
                             : (target + KEY_INTERVAL - 1) / KEY_INTERVAL * KEY_INTERVAL};
    ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(target)), preceeding));
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(key)));
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(key + 1)));
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
