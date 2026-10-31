/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AdaptiveByteStream.h"
#include "TestAdaptiveStream.h"
#include "mpegts/ES_AAC.h"
#include "mpegts/ES_AC3.h"
#include "mpegts/ES_MPEGAudio.h"
#include "mpegts/ES_MPEGVideo.h"
#include "mpegts/ES_h264.h"
#include "mpegts/ES_hevc.h"
#include "samplereader/TSSampleReader.h"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
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

// An E-AC-3 frame of 128 bytes: independent (0) or dependent (1), 48 kHz,
// 2 channels, bsid 16, with 1, 2, 3 or 6 blocks (blocksCode 0 to 3). The last
// two bytes hold the index.
std::vector<uint8_t> Eac3Frame(int frameType, int blocksCode = 3, int index = 0,
                               unsigned int words = 64)
{
  std::vector<uint8_t> frame{0x0B,
                             0x77,
                             static_cast<uint8_t>((frameType << 6) | ((words - 1) >> 8)),
                             static_cast<uint8_t>(words - 1),
                             static_cast<uint8_t>((blocksCode << 4) | 0x04),
                             0x80};
  frame.resize(2 * words, 0x55);
  frame[frame.size() - 2] = static_cast<uint8_t>(index >> 8);
  frame[frame.size() - 1] = static_cast<uint8_t>(index);
  return frame;
}

// A 7.1 E-AC-3 access unit: an independent and a dependent frame of 6 blocks
std::vector<uint8_t> Eac3AccessUnit(int index)
{
  std::vector<uint8_t> unit{Eac3Frame(0, 3, index)};
  const std::vector<uint8_t> dependent{Eac3Frame(1, 3, index)};
  unit.insert(unit.end(), dependent.begin(), dependent.end());
  return unit;
}
constexpr uint64_t EAC3_TICKS{2880}; // 1536 samples at 48 kHz

// The PMT stream type, the frames and the frame duration of an audio stream
struct AudioCodec
{
  uint8_t streamType;
  std::vector<uint8_t> (*frame)(int index);
  uint64_t ticks;
};

const AudioCodec AAC{0x0F, [](int index) { return AacFrame(index); }, AUDIO_TICKS};
const AudioCodec EAC3{0x87, [](int index) { return Eac3Frame(0, 3, index); }, EAC3_TICKS};
const AudioCodec EAC3_71{0x87, Eac3AccessUnit, EAC3_TICKS};

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

// The parameter sets of the key frame: the VPS, SPS and PPS, or the MPEG-2
// sequence header and sequence extension. Or a key frame that uses the
// parameter sets of an earlier key frame: the access unit without them.
std::vector<uint8_t> KeyFrameUnits(const VideoCodec& codec, bool parameterSets)
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
    if (parameterSet == parameterSets)
      result.insert(result.end(), au.begin() + starts[n], au.begin() + starts[n + 1]);
  }
  return result;
}

// The start of the first picture unit of an access unit: a slice NAL unit or
// an MPEG-2 picture
size_t PictureStart(const VideoCodec& codec, const std::vector<uint8_t>& au)
{
  for (size_t i = 0; i + 3 < au.size(); ++i)
  {
    if (au[i] != 0 || au[i + 1] != 0 || au[i + 2] != 1)
      continue;
    bool picture;
    if (codec.streamType == H264.streamType)
      picture = (au[i + 3] & 0x1F) >= 1 && (au[i + 3] & 0x1F) <= 5;
    else if (codec.streamType == HEVC.streamType)
      picture = ((au[i + 3] >> 1) & 0x3F) <= 31;
    else
      picture = au[i + 3] == 0;
    if (picture)
      return i > 0 && au[i - 1] == 0 ? i - 1 : i;
  }
  return au.size();
}

// The stream information of a parser
struct ParsedInfo
{
  std::vector<uint8_t> extraData;
  int width;
  int height;
  bool operator==(const ParsedInfo& other) const
  {
    return extraData == other.extraData && width == other.width && height == other.height;
  }
};

std::unique_ptr<TSDemux::ElementaryStream> MakeParser(const VideoCodec& codec)
{
  if (codec.streamType == H264.streamType)
    return std::make_unique<TSDemux::ES_h264>(VIDEO_PID);
  if (codec.streamType == HEVC.streamType)
    return std::make_unique<TSDemux::ES_hevc>(VIDEO_PID);
  return std::make_unique<TSDemux::ES_MPEG2Video>(VIDEO_PID);
}

// Parses access units, each in its own PES with a PTS. The parser gets the
// data in chunks, as from TS packets. Optionally the data ends at a cut in an
// access unit, as at the end of a segment, and the next data continues there.
ParsedInfo ParseAccessUnits(const VideoCodec& codec,
                            const std::vector<std::vector<uint8_t>>& accessUnits,
                            size_t chunk,
                            std::optional<std::pair<size_t, size_t>> cut = std::nullopt)
{
  const std::unique_ptr<TSDemux::ElementaryStream> stream{MakeParser(codec)};
  TSDemux::STREAM_PKT pkt;
  for (size_t n = 0; n < accessUnits.size(); ++n)
  {
    stream->c_pts = stream->c_dts = 90000 + 3000 * n;
    stream->StartPes(n, 0);
    const std::vector<uint8_t>& au{accessUnits[n]};
    std::vector<size_t> ends;
    for (size_t end = chunk; end < au.size(); end += chunk)
      ends.push_back(end);
    if (cut && cut->first == n)
      ends.push_back(cut->second);
    ends.push_back(au.size());
    std::sort(ends.begin(), ends.end());
    size_t start{0};
    for (const size_t end : ends)
    {
      stream->Append(au.data() + start, end - start, start == 0);
      while (stream->GetStreamPacket(&pkt))
        ;
      if (cut && cut->first == n && cut->second == end)
      {
        while (stream->GetEndPacket(&pkt, false))
          ;
      }
      start = end;
    }
  }
  while (stream->GetEndPacket(&pkt, true))
    ;
  const TSDemux::STREAM_INFO& info{stream->stream_info};
  return {{info.extra_data, info.extra_data + info.extra_data_size}, info.width, info.height};
}

// Writes TS packets with continuity counters that continue across segments
class TSWriter
{
public:
  explicit TSWriter(const VideoCodec& codec = H264, const AudioCodec& audio = AAC)
    : m_codec(codec), m_audio(audio)
  {
  }

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
    pmt.insert(pmt.end(), {m_audio.streamType, static_cast<uint8_t>(0xE0 | (AUDIO_PID >> 8)),
                           static_cast<uint8_t>(AUDIO_PID), 0xF0, 0x00});
    WriteSection(PMT_PID, pmt);
  }

  // PES packets of framesPerPes audio frames. A PES without a length ends only
  // at the start of the next PES.
  void WriteAudio(uint64_t firstPts, int firstFrame, int count, int framesPerPes, bool pesLength)
  {
    for (int frame = firstFrame; frame < firstFrame + count; frame += framesPerPes)
    {
      std::vector<uint8_t> payload;
      for (int i = frame; i < frame + framesPerPes; ++i)
      {
        const std::vector<uint8_t> data{m_audio.frame(i)};
        payload.insert(payload.end(), data.begin(), data.end());
      }
      WritePes(AUDIO_PID, 0xC0, firstPts + (frame - firstFrame) * m_audio.ticks, payload,
               pesLength);
    }
  }

  // One AAC frame in a PES with or without a PTS
  void WriteAudioFrame(int frame, bool timestamped)
  {
    WritePes(AUDIO_PID, 0xC0,
             timestamped ? std::optional<uint64_t>{AudioPts(frame)} : std::nullopt,
             AacFrame(frame), false);
  }

  // Audio frames in PES packets of pesSize bytes, which can end inside a frame
  // or a frame header. A PES has the PTS of the first frame that starts in it.
  void WriteAudioInPesOfSize(int count, size_t pesSize)
  {
    std::vector<uint8_t> payload;
    std::vector<size_t> frameStarts;
    for (int i = 0; i < count; ++i)
    {
      frameStarts.push_back(payload.size());
      const std::vector<uint8_t> aac{AacFrame(i)};
      payload.insert(payload.end(), aac.begin(), aac.end());
    }
    for (size_t start = 0; start < payload.size(); start += pesSize)
    {
      const size_t end{std::min(start + pesSize, payload.size())};
      const auto frame{std::lower_bound(frameStarts.begin(), frameStarts.end(), start)};
      std::optional<uint64_t> pts;
      if (frame != frameStarts.end() && *frame < end)
        pts = AudioPts(static_cast<int>(frame - frameStarts.begin()));
      WritePes(AUDIO_PID, 0xC0, pts, {payload.begin() + start, payload.begin() + end}, false);
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

  // A PES of other audio data, firstPacket is the PES size in the first TS packet
  void WriteAudioPes(std::optional<uint64_t> pts,
                     const std::vector<uint8_t>& payload,
                     size_t firstPacket = 184)
  {
    WritePes(AUDIO_PID, 0xC0, pts, payload, false, firstPacket);
  }

  void WriteVideoFrameWithoutPts(int frame, const std::vector<uint8_t>& accessUnit)
  {
    WritePes(VIDEO_PID, 0xE0, std::nullopt, accessUnit, false);
  }

  // A frame can span two PES packets, the second without a PTS
  void WriteVideoFrameInTwoPes(int frame, const std::vector<uint8_t>& accessUnit)
  {
    const auto half{accessUnit.begin() + accessUnit.size() / 2};
    WritePes(VIDEO_PID, 0xE0, VideoPts(frame), {accessUnit.begin(), half}, false);
    WritePes(VIDEO_PID, 0xE0, std::nullopt, {half, accessUnit.end()}, false);
  }

  // GStreamer puts the units before the picture, as the parameter sets, in a
  // PES of their own with the PTS of the frame
  void WriteVideoFrameInTimestampedPes(int frame, const std::vector<uint8_t>& accessUnit)
  {
    const auto picture{accessUnit.begin() + PictureStart(m_codec, accessUnit)};
    if (picture != accessUnit.begin())
      WritePes(VIDEO_PID, 0xE0, VideoPts(frame), {accessUnit.begin(), picture}, false);
    WritePes(VIDEO_PID, 0xE0, VideoPts(frame), {picture, accessUnit.end()}, false);
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
  const AudioCodec& m_audio;
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

// A corrupt parameter set can have an id outside the range of ids. The parser
// ignores such a parameter set, so the frames and the stream information stay
// the same.
TEST_F(TSSampleReaderTest, AParameterSetWithAnIdOutOfRangeIsIgnored)
{
  // The PPS id 100000 and the H.264 SPS id 1000 as ue(v), and an H.264 slice
  // with the PPS id 100000
  const std::vector<uint8_t> h264Sets{0x00, 0x00, 0x00, 0x01, 0x68, 0x00, 0x00, 0xC3, 0x50,
                                      0xC8, 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x0A,
                                      0x00, 0x7D, 0x30, 0x00, 0x00, 0x01, 0x41, 0xC0, 0x00,
                                      0x30, 0xD4, 0x30};
  const std::vector<uint8_t> hevcSets{0x00, 0x00, 0x00, 0x01, 0x44, 0x01,
                                      0x00, 0x00, 0xC3, 0x50, 0xD0};
  for (const VideoCodec* codec : {&H264, &HEVC})
  {
    SCOPED_TRACE(codec->name);
    // After the access unit delimiter of a P-frame
    std::vector<uint8_t> frame{codec->frame};
    const std::vector<uint8_t>& sets{codec == &H264 ? h264Sets : hevcSets};
    frame.insert(frame.begin() + (codec == &H264 ? 6 : 7), sets.begin(), sets.end());
    TSWriter writer{*codec};
    writer.WriteTables(true);
    for (int pts = 0; pts < SEGMENT_VIDEO_FRAMES; ++pts)
      writer.WriteVideoFrame(pts, pts == 0 ? codec->keyFrame : pts == 2 ? frame : codec->frame);
    Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    kodi::addon::InputstreamInfo info;
    info.SetStreamType(INPUTSTREAM_TYPE_VIDEO);
    ASSERT_TRUE(m_reader->GetInformation(info));
    int frames{0};
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample(), ++frames)
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(frames)));
    EXPECT_EQ(frames, SEGMENT_VIDEO_FRAMES);
    EXPECT_FALSE(m_reader->GetInformation(info));
  }
}

// Zero bytes can follow an H.264 SPS in the stream, so the SPS can be longer
// than the buffer of the parser
TEST_F(TSSampleReaderTest, ALongSpsKeepsTheFrames)
{
  // Before the PPS
  std::vector<uint8_t> keyFrame{H264.keyFrame};
  keyFrame.insert(keyFrame.begin() + 31, 300, 0);
  TSWriter writer;
  writer.WriteTables(true);
  for (int pts = 0; pts < SEGMENT_VIDEO_FRAMES; ++pts)
    writer.WriteVideoFrame(pts, pts % 5 ? H264.frame : keyFrame);
  Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
  int frames{0};
  bool started{false};
  for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
       result = m_reader->ReadSample(), ++frames)
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(frames)));
  EXPECT_EQ(frames, SEGMENT_VIDEO_FRAMES);
}

// TS packets split the parameter sets, and a segment can end in a parameter
// set. The stream information stays the same as from whole access units.
TEST(TSParserTest, ThePartsOfTheParameterSetsGiveTheSameStreamInformation)
{
  for (const VideoCodec* codec : {&H264, &HEVC, &MPEG2})
  {
    SCOPED_TRACE(codec->name);
    const std::vector<std::vector<uint8_t>> accessUnits{codec->keyFrame, codec->frame,
                                                        codec->keyFrame, codec->frame,
                                                        codec->frame};
    const ParsedInfo expected{ParseAccessUnits(*codec, accessUnits, SIZE_MAX)};
    ASSERT_NE(expected.width, 0);
    // The extra data has the parameter sets with 4 byte start codes, MPEG-2 has none
    ASSERT_EQ(expected.extraData,
              codec == &MPEG2 ? std::vector<uint8_t>{} : KeyFrameUnits(*codec, true));
    for (const size_t chunk : {size_t{1}, size_t{7}, size_t{184}})
    {
      SCOPED_TRACE(testing::Message() << "chunk " << chunk);
      EXPECT_EQ(ParseAccessUnits(*codec, accessUnits, chunk), expected);
      // The first key frame or the next key frame ends the data
      for (const size_t keyFrame : {size_t{0}, size_t{2}})
      for (size_t cut = 1; cut < codec->keyFrame.size(); ++cut)
      {
        SCOPED_TRACE(testing::Message() << "key frame " << keyFrame << ", cut " << cut);
        EXPECT_EQ(ParseAccessUnits(*codec, accessUnits, chunk, {{keyFrame, cut}}), expected);
      }
    }
  }
}

// A 3 byte start code can start a parameter set, also at the start of the
// data. The extra data has the parameter sets with 4 byte start codes.
// A seek can copy the last frame of the data, which the end of the data
// completed. The rest of that frame follows up to the next PES with a PTS, and
// after the reset of the seek, the stream drops that rest.
TEST(TSParserTest, TheRestOfACopiedLastFrameIsDropped)
{
  for (const VideoCodec* codec : {&H264, &HEVC, &MPEG2})
  {
    SCOPED_TRACE(codec->name);
    const std::unique_ptr<TSDemux::ElementaryStream> stream{MakeParser(*codec)};
    const std::vector<uint8_t>& key{codec->keyFrame};
    const size_t cut{PictureStart(*codec, key) + 8};
    std::vector<int64_t> read;
    TSDemux::STREAM_PKT pkt;
    const auto append = [&](int pes, const std::vector<uint8_t>& data, bool timestamped)
    {
      if (timestamped)
        stream->c_pts = stream->c_dts = 90000 + 3000 * pes;
      stream->StartPes(pes, 0);
      stream->Append(data.data(), data.size(), timestamped);
      while (stream->GetStreamPacket(&pkt))
        read.push_back(pkt.pts);
    };
    append(0, key, true);
    append(1, {key.begin(), key.begin() + cut}, true);
    while (stream->GetEndPacket(&pkt, false))
      read.push_back(pkt.pts);
    EXPECT_EQ(read, (std::vector<int64_t>{90000, 93000}));
    EXPECT_EQ(stream->NextFramePosition(), UINT64_MAX);

    stream->ContinueAfterCopy(stream->NextFramePosition(), true);
    stream->Reset();
    read.clear();
    // The rest of the copied frame, and a frame in the same PES without a PTS
    std::vector<uint8_t> rest{key.begin() + cut, key.end()};
    rest.insert(rest.end(), codec->frame.begin(), codec->frame.end());
    append(2, rest, false);
    append(3, codec->frame, true);
    append(4, codec->frame, true);
    while (stream->GetEndPacket(&pkt, true))
      read.push_back(pkt.pts);
    EXPECT_EQ(read, (std::vector<int64_t>{99000, 102000}));
  }
}

// A corrupt ADTS header can give a frame length of the header or less. A frame
// holds at least one raw data block after the header. The parser skips that
// header and finds the next frames.
TEST(TSParserTest, AnAdtsFrameWithoutDataIsSkipped)
{
  // The length of a header without a CRC, and of a header with a CRC
  for (const auto& [protectionAbsent, length] :
       {std::pair{true, 0U}, std::pair{true, 6U}, std::pair{true, 7U}, std::pair{false, 8U},
        std::pair{false, 9U}})
  {
    SCOPED_TRACE(testing::Message() << "length " << length << (protectionAbsent ? "" : ", CRC"));
    std::vector<uint8_t> data{AacFrame(0)};
    std::vector<uint8_t> corrupt{AacFrame(1)};
    corrupt[1] = protectionAbsent ? 0xF1 : 0xF0;
    corrupt[3] = static_cast<uint8_t>(0x80 | (length >> 11));
    corrupt[4] = static_cast<uint8_t>(length >> 3);
    corrupt[5] = static_cast<uint8_t>(((length & 7) << 5) | 0x1F);
    corrupt[6] = 0; // Else 0xFF of length 7 and this byte are a sync word
    corrupt.resize(std::max(length, 9U));
    data.insert(data.end(), corrupt.begin(), corrupt.end());
    for (int i = 2; i < 4; ++i)
    {
      const std::vector<uint8_t> frame{AacFrame(i)};
      data.insert(data.end(), frame.begin(), frame.end());
    }
    TSDemux::ES_AAC stream{AUDIO_PID};
    stream.stream_type = TSDemux::STREAM_TYPE_AUDIO_AAC;
    stream.c_pts = stream.c_dts = 90000;
    stream.StartPes(0, 0);
    stream.Append(data.data(), data.size(), true);
    std::vector<int> read;
    TSDemux::STREAM_PKT pkt;
    // A frame without data would come again and again
    for (int packets = 0; packets < 10 && stream.GetStreamPacket(&pkt); ++packets)
      read.push_back(pkt.size ? (pkt.data[pkt.size - 2] << 8) | pkt.data[pkt.size - 1] : -1);
    EXPECT_EQ(read, (std::vector<int>{0, 2, 3}));
  }
}

// A LATM frame of 3 + 120 bytes with the StreamMuxConfig of AAC LC at 48 kHz
// with 2 channels and 1 or 2 sub frames. The last two bytes hold the frame index.
std::vector<uint8_t> LatmFrame(int index, unsigned int length = 120, int subFrames = 1)
{
  std::vector<uint8_t> frame{0x56, static_cast<uint8_t>(0xE0 | (length >> 8)),
                             static_cast<uint8_t>(length),
                             // useSameStreamMux 0, audioMuxVersion 0, allStreamsSameTimeFraming 1,
                             // no sub frames, programs or layers, AOT 2, rate index 3,
                             // channels 2, frameLengthType 0, buffer fullness 0xFF
                             0x20, static_cast<uint8_t>((subFrames - 1) << 7), 0x11, 0x90,
                             0x1F, 0xE0};
  frame.resize(3 + 120, 0);
  frame[frame.size() - 2] = static_cast<uint8_t>(index >> 8);
  frame[frame.size() - 1] = static_cast<uint8_t>(index);
  return frame;
}

// A LATM frame holds an AudioMuxElement after the 3 header bytes. The parser
// skips a header with a length of 0 and finds the next frames.
TEST(TSParserTest, ALatmFrameWithoutDataIsSkipped)
{
  std::vector<uint8_t> data;
  for (int i = 0; i < 4; ++i)
  {
    const std::vector<uint8_t> frame{LatmFrame(i, i == 1 ? 0 : 120)};
    data.insert(data.end(), frame.begin(), frame.end());
  }
  TSDemux::ES_AAC stream{AUDIO_PID};
  stream.stream_type = TSDemux::STREAM_TYPE_AUDIO_AAC;
  stream.c_pts = stream.c_dts = 90000;
  stream.StartPes(0, 0);
  stream.Append(data.data(), data.size(), true);
  std::vector<std::pair<int, int64_t>> read;
  TSDemux::STREAM_PKT pkt;
  while (read.size() < 10 && (stream.GetStreamPacket(&pkt) || stream.GetEndPacket(&pkt, true)))
    read.emplace_back(pkt.size == 123 ? (pkt.data[121] << 8) | pkt.data[122] : -1, pkt.pts);
  EXPECT_EQ(read, (std::vector<std::pair<int, int64_t>>{
                      {0, 90000}, {2, 90000 + AUDIO_TICKS}, {3, 90000 + 2 * AUDIO_TICKS}}));
}


struct AudioFrame
{
  unsigned int size;
  int64_t pts;
  int64_t duration;
  bool operator==(const AudioFrame& other) const
  {
    return size == other.size && pts == other.pts && duration == other.duration;
  }
};

std::ostream& operator<<(std::ostream& os, const AudioFrame& frame)
{
  return os << "{" << frame.size << ", " << frame.pts << ", " << frame.duration << "}";
}

// The frames of one PES with the PTS 90000. The data comes in parts of the
// chunk size, as in TS packets.
std::vector<AudioFrame> ReadAudioFrames(TSDemux::ElementaryStream& stream,
                                        const std::vector<uint8_t>& data,
                                        size_t chunk = SIZE_MAX)
{
  stream.c_pts = stream.c_dts = 90000;
  stream.StartPes(0, 0);
  std::vector<AudioFrame> read;
  TSDemux::STREAM_PKT pkt;
  for (size_t pos = 0; pos < data.size(); pos += chunk)
  {
    stream.Append(data.data() + pos, std::min(chunk, data.size() - pos), pos == 0);
    while (read.size() < 10 && stream.GetStreamPacket(&pkt))
      read.push_back(
          {static_cast<unsigned int>(pkt.size), pkt.pts, static_cast<int64_t>(pkt.duration)});
  }
  while (read.size() < 10 && stream.GetEndPacket(&pkt, true))
    read.push_back(
        {static_cast<unsigned int>(pkt.size), pkt.pts, static_cast<int64_t>(pkt.duration)});
  return read;
}

// An ADTS frame holds 1 to 4 raw data blocks of 1024 samples, and a LATM
// frame 1 to 64 sub frames of 1024 samples. An ADTS header without a sample
// rate, as a false sync word in the data, is no header.
TEST(TSParserTest, AacFramesHaveTheDurationOfTheirBlocks)
{
  for (const int blocks : {1, 2, 4})
  {
    SCOPED_TRACE(testing::Message() << "ADTS, " << blocks << " raw data blocks");
    std::vector<uint8_t> data;
    for (int i = 0; i < 3; ++i)
    {
      std::vector<uint8_t> frame{AacFrame(i)};
      frame[6] = static_cast<uint8_t>(0xFC | (blocks - 1));
      data.insert(data.end(), frame.begin(), frame.end());
      if (i == 0)
      {
        // Rate index 13. A buffer fullness of 0 gives no other sync word.
        frame[2] = 0x74;
        frame[5] &= 0xE0;
        frame[6] &= 0x03;
        data.insert(data.end(), frame.begin(), frame.begin() + 7);
      }
    }
    TSDemux::ES_AAC stream{AUDIO_PID};
    stream.stream_type = TSDemux::STREAM_TYPE_AUDIO_AAC;
    const int64_t duration{blocks * static_cast<int64_t>(AUDIO_TICKS)};
    EXPECT_EQ(ReadAudioFrames(stream, data),
              (std::vector<AudioFrame>{{127, 90000, duration},
                                       {127, 90000 + duration, duration},
                                       {127, 90000 + 2 * duration, duration}}));
  }
  for (const int subFrames : {1, 2})
  {
    SCOPED_TRACE(testing::Message() << "LATM, " << subFrames << " sub frames");
    std::vector<uint8_t> data;
    for (int i = 0; i < 3; ++i)
    {
      const std::vector<uint8_t> frame{LatmFrame(i, 120, subFrames)};
      data.insert(data.end(), frame.begin(), frame.end());
    }
    TSDemux::ES_AAC stream{AUDIO_PID};
    stream.stream_type = TSDemux::STREAM_TYPE_AUDIO_AAC;
    const int64_t duration{subFrames * static_cast<int64_t>(AUDIO_TICKS)};
    EXPECT_EQ(ReadAudioFrames(stream, data),
              (std::vector<AudioFrame>{{123, 90000, duration},
                                       {123, 90000 + duration, duration},
                                       {123, 90000 + 2 * duration, duration}}));
  }
}

// The header gives the size, the duration and the channels of an MPEG audio
// frame. A Layer I frame holds 384 samples, an MPEG-2 or MPEG-2.5 Layer III
// frame 576 samples and the other frames 1152 samples.
TEST(TSParserTest, MpegAudioFramesHaveTheSizeAndDurationOfTheirHeader)
{
  struct Format
  {
    const char* name;
    uint8_t version; // 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5
    uint8_t layer; // 3 Layer I, 2 Layer II, 1 Layer III
    uint8_t bitrateIndex;
    uint8_t sampleRateIndex;
    unsigned int size;
    int64_t duration;
  };
  for (const Format& format : {
           // 128 kbit/s at 48 kHz
           Format{"MPEG-1 Layer I", 3, 3, 4, 1, 128, 720},
           Format{"MPEG-1 Layer II", 3, 2, 8, 1, 384, 2160},
           Format{"MPEG-1 Layer III", 3, 1, 9, 1, 384, 2160},
           // 64 kbit/s at 24 kHz, and at 12 kHz
           Format{"MPEG-2 Layer II", 2, 2, 8, 1, 384, 4320},
           Format{"MPEG-2 Layer III", 2, 1, 8, 1, 192, 2160},
           Format{"MPEG-2.5 Layer III", 0, 1, 8, 1, 384, 4320},
       })
  for (const int channels : {1, 2})
  {
    SCOPED_TRACE(testing::Message() << format.name << ", " << channels << " channels");
    std::vector<uint8_t> frame{0xFF,
                               static_cast<uint8_t>(0xE1 | (format.version << 3) |
                                                    (format.layer << 1)),
                               static_cast<uint8_t>((format.bitrateIndex << 4) |
                                                    (format.sampleRateIndex << 2)),
                               static_cast<uint8_t>(channels == 1 ? 0xC0 : 0x00)};
    frame.resize(format.size, 0x55);
    std::vector<uint8_t> data;
    for (int i = 0; i < 3; ++i)
      data.insert(data.end(), frame.begin(), frame.end());
    TSDemux::ES_MPEG2Audio stream{AUDIO_PID};
    const unsigned int size{format.size};
    const int64_t duration{format.duration};
    EXPECT_EQ(ReadAudioFrames(stream, data),
              (std::vector<AudioFrame>{{size, 90000, duration},
                                       {size, 90000 + duration, duration},
                                       {size, 90000 + 2 * duration, duration}}));
    EXPECT_EQ(stream.stream_info.channels, channels);
  }
}

// An E-AC-3 frame holds 1, 2, 3 or 6 blocks of 256 samples. A dependent frame
// extends the channels of the frame before, at the same time, so the decoder
// needs both frames in one packet.
TEST(TSParserTest, EAc3FramesHaveTheDurationOfTheirBlocks)
{
  constexpr int64_t BLOCK{480}; // 256 samples at 48 kHz
  for (const auto& [blocksCode, blocks] : {std::pair{0, 1}, std::pair{1, 2}, std::pair{2, 3},
                                           std::pair{3, 6}})
  {
    SCOPED_TRACE(testing::Message() << blocks << " blocks");
    // The first dependent frame belongs to a frame before the data
    std::vector<uint8_t> data;
    for (const int frameType : {1, 0, 1, 0, 1, 0})
    {
      const std::vector<uint8_t> frame{Eac3Frame(frameType, blocksCode)};
      data.insert(data.end(), frame.begin(), frame.end());
    }
    const int64_t duration{blocks * BLOCK};
    // The data of a frame, or of the header after the frame, can come later
    for (const size_t chunk : {SIZE_MAX, size_t{130}, size_t{131}, size_t{184}})
    {
      SCOPED_TRACE(testing::Message() << "parts of " << chunk << " bytes");
      TSDemux::ES_AC3 stream{AUDIO_PID};
      EXPECT_EQ(ReadAudioFrames(stream, data, chunk),
                (std::vector<AudioFrame>{{256, 90000, duration},
                                         {256, 90000 + duration, duration},
                                         {128, 90000 + 2 * duration, duration}}));
    }
  }
  // A dependent header with the reserved sample rate codes, with a frame size
  // of 2 bytes, or with the bsid 8 of AC-3, is no header, so the frame after
  // that header is a separate frame
  std::vector<uint8_t> data;
  for (int i = 0; i < 4; ++i)
  {
    const std::vector<uint8_t> frame{Eac3Frame(0)};
    data.insert(data.end(), frame.begin(), frame.end());
    if (i == 0)
      data.insert(data.end(), {0x0B, 0x77, 0x40, 0x3F, 0xF4, 0x80});
    else if (i == 1)
      data.insert(data.end(), {0x0B, 0x77, 0x40, 0x00, 0x34, 0x80});
    else if (i == 2)
      data.insert(data.end(), {0x0B, 0x77, 0x40, 0x3F, 0x34, 0x40});
  }
  TSDemux::ES_AC3 stream{AUDIO_PID};
  EXPECT_EQ(ReadAudioFrames(stream, data),
            (std::vector<AudioFrame>{{128, 90000, EAC3_TICKS},
                                     {128, 90000 + EAC3_TICKS, EAC3_TICKS},
                                     {128, 90000 + 2 * EAC3_TICKS, EAC3_TICKS},
                                     {128, 90000 + 3 * EAC3_TICKS, EAC3_TICKS}}));
}

// Dependent frames without the frame before, as at the start of the data,
// are no frames, and the frames after them come
TEST(TSParserTest, EAc3DependentFramesWithoutAFrameBeforeAreSkipped)
{
  for (const int orphans : {1, 2, 3})
  {
    std::vector<uint8_t> data;
    for (int i = 0; i < orphans + 3; ++i)
    {
      const std::vector<uint8_t> frame{Eac3Frame(i < orphans ? 1 : 0)};
      data.insert(data.end(), frame.begin(), frame.end());
    }
    for (const size_t chunk : {SIZE_MAX, size_t{128}, size_t{184}})
    {
      SCOPED_TRACE(testing::Message() << orphans << " dependent frames, parts of " << chunk
                                      << " bytes");
      TSDemux::ES_AC3 stream{AUDIO_PID};
      EXPECT_EQ(ReadAudioFrames(stream, data, chunk),
                (std::vector<AudioFrame>{{128, 90000, EAC3_TICKS},
                                         {128, 90000 + EAC3_TICKS, EAC3_TICKS},
                                         {128, 90000 + 2 * EAC3_TICKS, EAC3_TICKS}}));
    }
  }
}

// The end of a segment can come between the frames of an E-AC-3 access unit,
// or inside the dependent frame. The next segment can continue the access
// unit, so the parser keeps the frame for the rest. At the end of the stream,
// the frame comes alone, without the dependent frame that the stream cuts.
TEST(TSParserTest, AnEAc3AccessUnitWaitsForTheRestAtTheEndOfASegment)
{
  for (const size_t split : {size_t{128}, size_t{129}, size_t{130}, size_t{134}, size_t{200}})
  {
    std::vector<uint8_t> data{Eac3AccessUnit(0)};
    const std::vector<uint8_t> next{Eac3AccessUnit(1)};
    data.insert(data.end(), next.begin(), next.end());
    const auto readFrames = [](TSDemux::ES_AC3& stream, std::vector<AudioFrame>& read,
                               std::optional<bool> endOfStream)
    {
      TSDemux::STREAM_PKT pkt;
      while (read.size() < 10 &&
             (stream.GetStreamPacket(&pkt) ||
              (endOfStream && stream.GetEndPacket(&pkt, *endOfStream))))
        read.push_back(
            {static_cast<unsigned int>(pkt.size), pkt.pts, static_cast<int64_t>(pkt.duration)});
    };
    {
      SCOPED_TRACE(testing::Message() << "segment end after " << split << " bytes");
      TSDemux::ES_AC3 stream{AUDIO_PID};
      stream.c_pts = stream.c_dts = 90000;
      stream.StartPes(0, 0);
      stream.Append(data.data(), split, true);
      std::vector<AudioFrame> read;
      readFrames(stream, read, false);
      stream.Append(data.data() + split, data.size() - split, false);
      readFrames(stream, read, true);
      EXPECT_EQ(read, (std::vector<AudioFrame>{{256, 90000, EAC3_TICKS},
                                               {256, 90000 + EAC3_TICKS, EAC3_TICKS}}));
    }
    SCOPED_TRACE(testing::Message() << "stream end after " << split << " bytes");
    TSDemux::ES_AC3 stream{AUDIO_PID};
    stream.c_pts = stream.c_dts = 90000;
    stream.StartPes(0, 0);
    stream.Append(data.data(), split, true);
    std::vector<AudioFrame> read;
    readFrames(stream, read, true);
    EXPECT_EQ(read, (std::vector<AudioFrame>{{128, 90000, EAC3_TICKS}}));
  }
  // Other data after the frame
  std::vector<uint8_t> data{Eac3Frame(0)};
  data.insert(data.end(), {0x00, 0x00, 0x00});
  TSDemux::ES_AC3 stream{AUDIO_PID};
  EXPECT_EQ(ReadAudioFrames(stream, data), (std::vector<AudioFrame>{{128, 90000, EAC3_TICKS}}));
}

TEST(TSParserTest, TheExtraDataHasTheParameterSetsAfter3ByteStartCodes)
{
  const auto shortStartCodes = [](const std::vector<uint8_t>& au) {
    std::vector<uint8_t> result;
    for (size_t i = 0; i < au.size(); ++i)
    {
      if (i + 3 < au.size() && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1)
        continue;
      result.push_back(au[i]);
    }
    return result;
  };
  // Without the access unit delimiter, the VPS starts the data
  const std::vector<uint8_t> keyFrame{shortStartCodes(
      std::vector<uint8_t>(HEVC.keyFrame.begin() + 7, HEVC.keyFrame.end()))};
  const std::vector<uint8_t> frame{shortStartCodes(HEVC.frame)};
  const ParsedInfo expected{ParseAccessUnits(HEVC, {HEVC.keyFrame, HEVC.frame}, SIZE_MAX)};
  EXPECT_EQ(ParseAccessUnits(HEVC, {keyFrame, frame}, SIZE_MAX), expected);
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
    const std::vector<uint8_t> key{KeyFrameUnits(*codec, false)};
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
  const std::vector<uint8_t> iPicture{KeyFrameUnits(MPEG2, false)};
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

// A frame can span two PES packets with the PTS of the frame, the parameter
// sets and the picture. At the end of the stream, the last frame is read whole.
TEST_F(TSSampleReaderTest, AFrameInTwoTimestampedPesPacketsIsReadWholeAtTheEndOfTheStream)
{
  for (const VideoCodec* codec : {&H264, &HEVC, &MPEG2})
  {
    SCOPED_TRACE(codec->name);
    TSWriter writer{*codec};
    writer.WriteTables(true);
    for (int frame = 0; frame < SEGMENT_VIDEO_FRAMES; ++frame)
      writer.WriteVideoFrameInTimestampedPes(frame, frame % 5 ? codec->frame : codec->keyFrame);
    Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    std::vector<uint64_t> read;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
    {
      SCOPED_TRACE(read.size());
      read.push_back(m_reader->PTS());
      EXPECT_GE(m_reader->GetSampleDataSize(),
                ((read.size() - 1) % 5 ? codec->frame : codec->keyFrame).size() - 1);
    }
    std::vector<uint64_t> expected;
    for (int i = 0; i < SEGMENT_VIDEO_FRAMES; ++i)
      expected.push_back(Time(VideoPts(i)));
    EXPECT_EQ(read, expected);
    EXPECT_TRUE(m_reader->EOS());
  }
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
// also when the seek waits for the next live segment. The P-frames after that
// key frame follow. The stream information stays the same, also when the
// segment ends in a parameter set.
TEST_F(TSSampleReaderTest, SeekLandsOnAKeyFrameAcrossTwoSegments)
{
  constexpr int SPLIT_FRAME{SEGMENT_VIDEO_FRAMES - 1};
  // The first packet ends in the PES header, in the parameter sets or in the frame
  for (const VideoCodec* codec : {&H264, &HEVC, &MPEG2})
  for (size_t firstPacket = 1; firstPacket <= 184; ++firstPacket)
  {
    std::vector<uint8_t> bigKeyFrame{codec->keyFrame};
    bigKeyFrame.resize(500, 0);
    TSWriter writer{*codec};
    writer.WriteTables(true);
    writer.WriteVideo(0, SPLIT_FRAME, 5);
    std::vector<uint8_t> first{writer.Take()};
    writer.WriteVideoFrame(SPLIT_FRAME, bigKeyFrame, firstPacket);
    const std::vector<uint8_t> split{writer.Take()};
    first.insert(first.end(), split.begin(), split.begin() + 188);
    writer.WriteTables(true);
    std::vector<uint8_t> second{writer.Take()};
    second.insert(second.end(), split.begin() + 188, split.end());
    // P-frames follow the key frame, up to the key frame 20
    writer.WriteVideo(SEGMENT_VIDEO_FRAMES, SEGMENT_VIDEO_FRAMES, 10);
    const std::vector<uint8_t> rest{writer.Take()};
    second.insert(second.end(), rest.begin(), rest.end());

    for (const bool live : {false, true})
    for (const bool preceeding : {true, false})
    for (const uint64_t target : {VideoPts(SPLIT_FRAME - 2), VideoPts(SPLIT_FRAME),
                                  VideoPts(SPLIT_FRAME) + VIDEO_TICKS / 2})
    {
      SCOPED_TRACE(testing::Message() << codec->name << ", first packet " << firstPacket
                                      << ", target " << target
                                      << (preceeding ? "" : ", not preceeding")
                                      << (live ? ", live" : ""));
      const int landing{preceeding && target < VideoPts(SPLIT_FRAME) ? 10
                        : !preceeding && target > VideoPts(SPLIT_FRAME) ? 20
                                                                         : SPLIT_FRAME};
      // The live seeks read the next segment for the landing frame
      if (live && landing < SPLIT_FRAME)
        continue;
      Open(first, Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
      kodi::addon::InputstreamInfo info;
      info.SetStreamType(INPUTSTREAM_TYPE_VIDEO);
      ASSERT_TRUE(m_reader->GetInformation(info));
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
      EXPECT_FALSE(m_reader->GetInformation(info));
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
      EXPECT_FALSE(m_reader->GetInformation(info));
    }
  }
}

// The parameter sets and the picture of a key frame can come in two PES
// packets with the PTS of the frame, and the segment can end after the first
// TS packets of the frame. A seek to that key frame returns the whole frame,
// and the frame before does not take the parameter sets.
TEST_F(TSSampleReaderTest, SeekLandsOnAKeyFrameInTwoTimestampedPesAcrossTwoSegments)
{
  constexpr int SPLIT_FRAME{SEGMENT_VIDEO_FRAMES - 1};
  for (const VideoCodec* codec : {&H264, &HEVC, &MPEG2})
  {
    std::vector<uint8_t> bigKeyFrame{codec->keyFrame};
    bigKeyFrame.resize(500, 0);
    TSWriter writer{*codec};
    writer.WriteTables(true);
    for (int frame = 0; frame < SPLIT_FRAME; ++frame)
      writer.WriteVideoFrameInTimestampedPes(frame, frame % 5 ? codec->frame : codec->keyFrame);
    const std::vector<uint8_t> frames{writer.Take()};
    writer.WriteVideoFrameInTimestampedPes(SPLIT_FRAME, bigKeyFrame);
    const std::vector<uint8_t> split{writer.Take()};
    writer.WriteTables(true);
    const std::vector<uint8_t> tables{writer.Take()};
    for (int frame = SEGMENT_VIDEO_FRAMES; frame < 2 * SEGMENT_VIDEO_FRAMES; ++frame)
      writer.WriteVideoFrameInTimestampedPes(frame, frame % 5 ? codec->frame : codec->keyFrame);
    const std::vector<uint8_t> rest{writer.Take()};

    for (size_t cut = 188; cut < split.size(); cut += 188)
    for (const bool live : {false, true})
    {
      SCOPED_TRACE(testing::Message() << codec->name << ", cut " << cut << (live ? ", live" : ""));
      std::vector<uint8_t> first{frames};
      first.insert(first.end(), split.begin(), split.begin() + cut);
      std::vector<uint8_t> second{tables};
      second.insert(second.end(), split.begin() + cut, split.end());
      second.insert(second.end(), rest.begin(), rest.end());

      Open(first, Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
      if (live)
        m_segments->m_waitingForSegment = true;
      else
        AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
      m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
      m_segments->m_segmentReadPos = 0;
      m_reader->Reset(false);
      ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(SPLIT_FRAME)), true));
      if (live)
      {
        AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
        m_segments->m_waitingForSegment = false;
        ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
      }
      std::vector<uint64_t> read;
      do
      {
        const int frame{SPLIT_FRAME + static_cast<int>(read.size())};
        SCOPED_TRACE(frame);
        read.push_back(m_reader->PTS());
        EXPECT_GE(m_reader->GetSampleDataSize(),
                  (frame == SPLIT_FRAME ? bigKeyFrame
                   : frame % 5           ? codec->frame
                                         : codec->keyFrame)
                          .size() -
                      1);
      } while (AP4_SUCCEEDED(m_reader->ReadSample()));
      std::vector<uint64_t> expected;
      for (int frame = SPLIT_FRAME; frame < 2 * SEGMENT_VIDEO_FRAMES; ++frame)
        expected.push_back(Time(VideoPts(frame)));
      EXPECT_EQ(read, expected);
    }
  }
}

// The last video and audio PES of a segment can continue in the next segment.
// After a seek to the key frame of that video PES, the audio of that audio PES
// follows the key frame, as the stream cannot read the PES again. This is also
// true when the seek waits for the next live segment.
TEST_F(TSSampleReaderTest, SeekToAKeyFrameAcrossTwoSegmentsKeepsTheAudioAcrossTheSegments)
{
  constexpr int SPLIT_FRAME{SEGMENT_VIDEO_FRAMES - 1};
  constexpr int SPLIT_AUDIO{2 * SEGMENT_VIDEO_FRAMES};
  constexpr int SPLIT_FRAMES{4};
  // The audio of the next segment comes before or after the video of a frame.
  // With the video first, the rest of the audio PES comes after the next video PES.
  // A live seek waits for the next segment, also over more reads. The first
  // segment holds one or more frames of the audio PES. With 0 audio bytes, the
  // first TS packet ends after the first frame of the PES, between the two
  // frames of an E-AC-3 access unit.
  for (const AudioCodec* codec : {&AAC, &EAC3_71})
  for (const size_t audioSplit : {size_t{0}, size_t{188}, size_t{376}})
  for (const bool live : {false, true})
  for (const int waitReads : {0, 2})
  for (const bool audioFirst : {true, false})
  for (const bool preceeding : {true, false})
  {
    if ((!live && waitReads) || (!audioSplit && codec != &EAC3_71))
      continue;
    SCOPED_TRACE(testing::Message() << (codec == &AAC ? "AAC, " : "7.1 E-AC-3, ") << audioSplit
                                    << " audio bytes, "
                                    << (audioFirst ? "audio first" : "video first")
                                    << (preceeding ? "" : ", not preceeding")
                                    << (live ? ", live" : "") << ", " << waitReads
                                    << " wait reads");
    const auto audioPts = [codec](int frame) { return BASE_PTS + frame * codec->ticks; };
    TSWriter writer{H264, *codec};
    writer.WriteTables(true);
    for (int v = 0; v < SPLIT_FRAME; ++v)
    {
      writer.WriteAudio(audioPts(2 * v) - AUDIO_LEAD, 2 * v, 2, 1, false);
      writer.WriteVideo(v, 1, 5);
    }
    writer.WriteAudio(audioPts(2 * SPLIT_FRAME) - AUDIO_LEAD, 2 * SPLIT_FRAME, 2, 1, false);
    std::vector<uint8_t> first{writer.Take()};
    // 9 of the 14 bytes of the PES header of the key frame
    writer.WriteVideoFrame(SPLIT_FRAME, H264.keyFrame, 9);
    const std::vector<uint8_t> keyFrame{writer.Take()};
    std::vector<uint8_t> payload;
    for (int a = SPLIT_AUDIO; a < SPLIT_AUDIO + SPLIT_FRAMES; ++a)
    {
      const std::vector<uint8_t> frame{codec->frame(a)};
      payload.insert(payload.end(), frame.begin(), frame.end());
    }
    // The PES header has 14 bytes
    writer.WriteAudioPes(audioPts(SPLIT_AUDIO) - AUDIO_LEAD, payload,
                         audioSplit ? 184 : 14 + Eac3Frame(0).size());
    const std::vector<uint8_t> audio{writer.Take()};
    const size_t firstBytes{audioSplit ? audioSplit : 188};
    first.insert(first.end(), keyFrame.begin(), keyFrame.begin() + 188);
    first.insert(first.end(), audio.begin(), audio.begin() + firstBytes);
    writer.WriteTables(true);
    std::vector<uint8_t> second{writer.Take()};
    second.insert(second.end(), keyFrame.begin() + 188, keyFrame.end());
    if (audioFirst)
      second.insert(second.end(), audio.begin() + firstBytes, audio.end());
    for (int v = SEGMENT_VIDEO_FRAMES; v < 2 * SEGMENT_VIDEO_FRAMES; ++v)
    {
      const int a{2 * v + SPLIT_FRAMES};
      if (!audioFirst)
        writer.WriteVideo(v, 1, 5);
      if (!audioFirst && v == SEGMENT_VIDEO_FRAMES)
      {
        const std::vector<uint8_t> video{writer.Take()};
        second.insert(second.end(), video.begin(), video.end());
        second.insert(second.end(), audio.begin() + firstBytes, audio.end());
      }
      writer.WriteAudio(audioPts(a) - AUDIO_LEAD, a, 2, 1, false);
      if (audioFirst)
        writer.WriteVideo(v, 1, 5);
    }
    const std::vector<uint8_t> rest{writer.Take()};
    second.insert(second.end(), rest.begin(), rest.end());

    Open(first, Time(VideoPts(0)) - SegmentDiff(0), INPUTSTREAM_TYPE_VIDEO);
    if (live)
      m_segments->m_waitingForSegment = true;
    else
      AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
    m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(SPLIT_FRAME) - (preceeding ? 0 : VIDEO_TICKS / 2)),
                                   preceeding));
    if (live)
    {
      EXPECT_FALSE(m_reader->EOS());
      for (int read = 0; read < waitReads; ++read)
      {
        EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);
        EXPECT_FALSE(m_reader->EOS());
      }
      AppendSegment(second, Time(VideoPts(SEGMENT_VIDEO_FRAMES)) - SegmentDiff(1));
      m_segments->m_waitingForSegment = false;
      ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    }
    ASSERT_EQ(m_reader->GetStreamId(), VIDEO_ID);
    EXPECT_EQ(m_reader->PTS(), Time(VideoPts(SPLIT_FRAME)));
    EXPECT_EQ(m_reader->GetPTSDiff(), SegmentDiff(0));
    // The audio frames, the PTS diff and the size of each
    std::vector<std::tuple<int, int64_t, AP4_Size>> read;
    int video{SPLIT_FRAME + 1};
    while (read.size() < 2 * SPLIT_FRAMES + 2 && AP4_SUCCEEDED(m_reader->ReadSample()))
    {
      if (m_reader->GetStreamId() == VIDEO_ID)
        EXPECT_EQ(m_reader->PTS(), Time(VideoPts(video++)));
      else
        read.emplace_back(FrameIndex(), m_reader->GetPTSDiff(), m_reader->GetSampleDataSize());
    }
    // The seek read the first video PES of the next segment, so all audio of
    // that segment takes the diff of that segment
    std::vector<std::tuple<int, int64_t, AP4_Size>> expected;
    for (int a = SPLIT_AUDIO; a < SPLIT_AUDIO + 2 * SPLIT_FRAMES + 2; ++a)
      expected.emplace_back(a, SegmentDiff(a < SPLIT_AUDIO + SPLIT_FRAMES ? 0 : 1),
                            static_cast<AP4_Size>(codec->frame(a).size()));
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

// A PES of audio can end inside a frame or inside a frame header. The parser
// finds a frame only after more data, so the next PES headers came before.
// Each frame still takes the PTS of the PES where the frame starts.
TEST_F(TSSampleReaderTest, AudioFramesAcrossPesPacketsTakeTheirPts)
{
  for (size_t pesSize = 1; pesSize <= 2 * AacFrame(0).size() + 8; ++pesSize)
  {
    SCOPED_TRACE(pesSize);
    TSWriter writer;
    writer.WriteTables(false);
    writer.WriteAudioInPesOfSize(SEGMENT_FRAMES, pesSize);
    Open(writer.Take(), Time(AudioPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
    std::vector<std::pair<int, uint64_t>> read;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
      read.emplace_back(FrameIndex(), m_reader->PTS());
    std::vector<std::pair<int, uint64_t>> expected;
    for (int i = 0; i < SEGMENT_FRAMES; ++i)
      expected.emplace_back(i, Time(AudioPts(i)));
    ASSERT_EQ(read, expected);
  }
}

// An audio PES needs a PTS only every 0.7 s. The frames of a PES without a PTS
// follow the frames before, also at the start and after a seek. The first
// audio PES after the seek frame can come after the next video PES.
TEST_F(TSSampleReaderTest, AudioOfPesWithoutPtsHasThePtsOfItsFrame)
{
  constexpr int VIDEO_FRAMES{30};
  for (const int firstPts : {0, 3})
  for (const int videoPerGroup : {1, 2})
  for (const std::optional<int> target : {std::optional<int>{}, std::optional<int>{0},
                                          std::optional<int>{10}})
  {
    SCOPED_TRACE(testing::Message() << "PTS in audio PES " << firstPts << " + 6n, "
                                    << videoPerGroup << " video frames before the audio, "
                                    << (target ? "seek to frame " + std::to_string(*target)
                                               : std::string{"no seek"}));
    TSWriter writer;
    writer.WriteTables(true);
    for (int v = 0; v < VIDEO_FRAMES; v += videoPerGroup)
    {
      writer.WriteVideo(v, videoPerGroup, 5);
      for (int a = 2 * v; a < 2 * (v + videoPerGroup); ++a)
        writer.WriteAudioFrame(a, a % 6 == firstPts);
    }
    Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
    m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    if (target)
    {
      m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
      m_segments->m_segmentReadPos = 0;
      m_reader->Reset(false);
      ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(*target)), true));
      ASSERT_EQ(m_reader->GetStreamId(), VIDEO_ID);
      EXPECT_EQ(m_reader->PTS(), Time(VideoPts(*target)));
    }
    std::vector<std::pair<int, uint64_t>> read;
    do
    {
      if (m_reader->GetStreamId() == AUDIO_ID)
        read.emplace_back(FrameIndex(), m_reader->PTS());
    } while (read.size() < 8 && AP4_SUCCEEDED(m_reader->ReadSample()));
    std::vector<std::pair<int, uint64_t>> expected;
    for (int a = 2 * target.value_or(0); a < 2 * target.value_or(0) + 8; ++a)
      expected.emplace_back(a, Time(AudioPts(a)));
    EXPECT_EQ(read, expected);
  }
}

// A seek scan knows the clock of the frames before the first PTS of the
// segment, so a seek lands on such a frame. Also at the stream start, and
// after a read into the next segment.
TEST_F(TSSampleReaderTest, AudioSeekLandsOnAFrameBeforeTheFirstPts)
{
  for (const bool nextSegment : {false, true})
  {
    SCOPED_TRACE(nextSegment ? "seek into the next segment" : "seek in the first segment");
    const int first{nextSegment ? 10 : 0};
    TSWriter writer;
    writer.WriteTables(false);
    if (nextSegment)
      writer.WriteAudio(AudioPts(0), 0, 10, 1, false);
    std::vector<uint8_t> segment{writer.Take()};
    writer.WriteTables(false);
    for (int a = first; a < first + 30; ++a)
      writer.WriteAudioFrame(a, a % 6 == (nextSegment ? 1 : 3));
    std::vector<uint8_t> second{writer.Take()};
    if (!nextSegment)
      segment = second;
    Open(segment, Time(AudioPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    if (nextSegment)
    {
      AppendSegment(second, Time(AudioPts(10)) - DIFF);
      while (m_segments->m_segments.size() > 1)
        ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
    }
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    ASSERT_TRUE(m_reader->TimeSeek(Time(AudioPts(first + 1)), true));
    EXPECT_EQ(FrameIndex(), first + 1);
    EXPECT_EQ(m_reader->PTS(), Time(AudioPts(first + 1)));
  }
}

// The first audio PTS can come in the next segment. The audio frames before
// wait for that PTS, the video frames do not wait.
TEST_F(TSSampleReaderTest, AudioBeforeAPtsInTheNextSegmentTakesThatClock)
{
  for (const bool video : {false, true})
  {
    SCOPED_TRACE(video ? "video and audio" : "audio");
    TSWriter writer;
    std::vector<uint8_t> segments[2];
    for (int s = 0; s < 2; ++s)
    {
      writer.WriteTables(video);
      for (int v = 5 * s; v < 5 * s + 5; ++v)
      {
        if (video)
          writer.WriteVideo(v, 1, 5);
        for (int a = 2 * v; a < 2 * v + 2; ++a)
          writer.WriteAudioFrame(a, a % 6 == 4 && s == 1);
      }
      segments[s] = writer.Take();
    }
    Open(segments[0], Time(AudioPts(0)) - DIFF,
         video ? INPUTSTREAM_TYPE_VIDEO : INPUTSTREAM_TYPE_AUDIO);
    AppendSegment(segments[1], Time(AudioPts(10)) - DIFF);
    if (video)
      m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
    std::vector<std::pair<int, uint64_t>> audio;
    std::vector<uint64_t> videoPts;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
    {
      if (m_reader->GetStreamId() == AUDIO_ID)
        audio.emplace_back(FrameIndex(), m_reader->PTS());
      else
        videoPts.push_back(m_reader->PTS());
    }
    std::vector<std::pair<int, uint64_t>> expected;
    for (int a = 0; a < 20; ++a)
      expected.emplace_back(a, Time(AudioPts(a)));
    EXPECT_EQ(audio, expected);
    std::vector<uint64_t> expectedVideo;
    for (int v = 0; video && v < 10; ++v)
      expectedVideo.push_back(Time(VideoPts(v)));
    EXPECT_EQ(videoPts, expectedVideo);
  }
}

// A seek inside the segment continues the read without a reset. The audio
// frames that wait for a PTS are from before the seek and do not come.
TEST_F(TSSampleReaderTest, AudioThatWaitsForAPtsDoesNotComeAfterASeek)
{
  TSWriter writer;
  writer.WriteTables(true);
  for (int v = 0; v < 20; ++v)
  {
    writer.WriteVideo(v, 1, 5);
    for (int a = 2 * v; a < 2 * v + 2; ++a)
      writer.WriteAudioFrame(a, a == 8);
  }
  Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
  m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
  // The video comes while the first audio frames wait
  ASSERT_EQ(m_reader->GetStreamId(), VIDEO_ID);
  ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(10)), true));
  EXPECT_EQ(m_reader->PTS(), Time(VideoPts(10)));
  std::vector<int> audio;
  while (AP4_SUCCEEDED(m_reader->ReadSample()))
  {
    if (m_reader->GetStreamId() == AUDIO_ID)
      audio.push_back(FrameIndex());
  }
  std::vector<int> expected;
  for (int a = 20; a < 40; ++a)
    expected.push_back(a);
  EXPECT_EQ(audio, expected);
}

// A stream must have a PTS at least every 0.7 s. Audio frames without a PTS
// for more than 1 s, or up to the end of the stream, come without a PTS, and
// the frames after them still come.
TEST_F(TSSampleReaderTest, AudioWithoutAPtsForTooLongComesWithoutPts)
{
  // 1 s holds 46.875 frames
  for (const int firstPts : {60, -1})
  {
    SCOPED_TRACE(firstPts < 0 ? "no PTS" : "first PTS in frame 60");
    TSWriter writer;
    writer.WriteTables(false);
    for (int a = 0; a < 70; ++a)
      writer.WriteAudioFrame(a, a == firstPts);
    Open(writer.Take(), Time(AudioPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
    std::vector<std::pair<int, uint64_t>> read;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
      read.emplace_back(FrameIndex(), m_reader->PTS());
    std::vector<std::pair<int, uint64_t>> expected;
    for (int a = 0; a < 70; ++a)
      expected.emplace_back(a, a < 47 || firstPts < 0 ? STREAM_NOPTS_VALUE : Time(AudioPts(a)));
    EXPECT_EQ(read, expected);
    EXPECT_TRUE(m_reader->EOS());
  }
}

// The held audio frames of short segments take the PTS diff of their segment,
// also of a segment more than a few segments before the first PTS
TEST_F(TSSampleReaderTest, HeldAudioOfShortSegmentsHasTheDiffOfItsSegment)
{
  TSWriter writer;
  for (int s = 0; s < 12; ++s)
  {
    writer.WriteTables(false);
    for (int a = 2 * s; a < 2 * s + 2; ++a)
      writer.WriteAudioFrame(a, a == 20);
    const uint64_t start{Time(AudioPts(2 * s)) - SegmentDiff(s)};
    if (s == 0)
      Open(writer.Take(), start, INPUTSTREAM_TYPE_AUDIO);
    else
      AppendSegment(writer.Take(), start);
  }
  std::vector<std::tuple<int, uint64_t, int64_t>> read;
  bool started{false};
  for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
       result = m_reader->ReadSample())
    read.emplace_back(FrameIndex(), m_reader->PTS(), m_reader->GetPTSDiff());
  std::vector<std::tuple<int, uint64_t, int64_t>> expected;
  for (int a = 0; a < 24; ++a)
    expected.emplace_back(a, Time(AudioPts(a)), SegmentDiff(a / 2));
  EXPECT_EQ(read, expected);
}

// In the last segment of the stream, the E-AC-3 parser does not wait for
// the next segment: a seek finds the last audio frame of the stream
TEST_F(TSSampleReaderTest, ASeekFindsTheLastAudioFrameOfTheStream)
{
  for (const AudioCodec* codec : {&AAC, &EAC3, &EAC3_71})
  for (const bool preceeding : {true, false})
  for (const int target : {8, 9})
  {
    SCOPED_TRACE(testing::Message() << (codec == &AAC ? "AAC" : codec == &EAC3 ? "E-AC-3" : "7.1 E-AC-3")
                                    << (preceeding ? "" : ", not preceeding") << ", seek to " << target);
    TSWriter writer{H264, *codec};
    writer.WriteTables(false);
    writer.WriteAudio(BASE_PTS, 0, 10, 1, false);
    Open(writer.Take(), Time(BASE_PTS) - DIFF, INPUTSTREAM_TYPE_AUDIO);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    ASSERT_TRUE(m_reader->TimeSeek(Time(BASE_PTS + target * codec->ticks), preceeding));
    EXPECT_FALSE(m_reader->EOS());
    EXPECT_EQ(FrameIndex(), target);
    EXPECT_EQ(m_reader->GetSampleDataSize(), codec->frame(target).size());
  }
}

// The only PTS of the last segment of the stream comes with the last access
// unit. The clock scan of a seek takes the clock from that PTS.
TEST_F(TSSampleReaderTest, ASeekTakesTheClockFromTheLastAudioFrameOfTheStream)
{
  for (const AudioCodec* codec : {&EAC3, &EAC3_71})
  for (const int target : {0, 5, 9})
  {
    SCOPED_TRACE(testing::Message() << (codec == &EAC3 ? "E-AC-3" : "7.1 E-AC-3") << ", seek to "
                                    << target);
    TSWriter writer{H264, *codec};
    writer.WriteTables(false);
    for (int a = 0; a < 10; ++a)
      writer.WriteAudioPes(a == 9 ? std::optional<uint64_t>{BASE_PTS + 9 * EAC3_TICKS}
                                  : std::nullopt,
                           codec->frame(a));
    Open(writer.Take(), Time(BASE_PTS) - DIFF, INPUTSTREAM_TYPE_AUDIO);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    ASSERT_EQ(FrameIndex(), 0);
    ASSERT_EQ(m_reader->PTS(), Time(BASE_PTS));
    m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
    m_segments->m_segmentReadPos = 0;
    m_reader->Reset(false);
    ASSERT_TRUE(m_reader->TimeSeek(Time(BASE_PTS + target * EAC3_TICKS), true));
    EXPECT_EQ(FrameIndex(), target);
    EXPECT_EQ(m_reader->PTS(), Time(BASE_PTS + target * EAC3_TICKS));
  }
}

// Audio frames without a PTS wait for the next PTS of the audio for at most
// 1 s of the stream. Without more audio, the time of the video counts.
TEST_F(TSSampleReaderTest, HeldAudioComesWithoutPtsAfterOneSecondOfVideo)
{
  TSWriter writer;
  writer.WriteTables(true);
  for (int a = 0; a < 3; ++a)
    writer.WriteAudioFrame(a, false);
  writer.WriteVideo(0, 40);
  // Frame 2 waits again
  writer.WriteAudioFrame(3, false);
  writer.WriteVideo(40, 40);
  Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
  m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
  std::vector<std::pair<AP4_UI32, uint64_t>> read;
  bool started{false};
  for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
       result = m_reader->ReadSample())
    read.emplace_back(m_reader->GetStreamId(), m_reader->PTS());
  // The DTS of video frame 24 is more than 1 s after the DTS of frame 0. The
  // last audio PES waits in the buffer for the next audio PES.
  std::vector<std::pair<AP4_UI32, uint64_t>> expected;
  for (int v = 0; v < 24; ++v)
    expected.emplace_back(VIDEO_ID, Time(VideoPts(v)));
  for (int a = 0; a < 2; ++a)
    expected.emplace_back(AUDIO_ID, STREAM_NOPTS_VALUE);
  // The parser reads frame 2 when the PES of frame 3 starts. The next video
  // frame that the reader returns is frame 38, and frame 62 is 1 s later.
  for (int v = 24; v < 62; ++v)
    expected.emplace_back(VIDEO_ID, Time(VideoPts(v)));
  expected.emplace_back(AUDIO_ID, STREAM_NOPTS_VALUE);
  for (int v = 62; v < 80; ++v)
    expected.emplace_back(VIDEO_ID, Time(VideoPts(v)));
  // Frame 3 comes at the end of the stream
  expected.emplace_back(AUDIO_ID, STREAM_NOPTS_VALUE);
  EXPECT_EQ(read, expected);
}

// A segment can end inside the header of the next PES. The frames of that
// PES take the PTS diff of that segment.
TEST_F(TSSampleReaderTest, APesHeaderAtTheEndOfASegmentKeepsTheDiffOfThatSegment)
{
  for (const AudioCodec* codec : {&AAC, &EAC3_71})
  for (const size_t headerBytes : {size_t{1}, size_t{9}, size_t{13}, size_t{14}})
  {
    SCOPED_TRACE(testing::Message() << (codec == &AAC ? "AAC, " : "7.1 E-AC-3, ") << headerBytes
                                    << " bytes of the PES header");
    TSWriter writer{H264, *codec};
    writer.WriteTables(false);
    for (int a = 0; a < 3; ++a)
      writer.WriteAudioPes(BASE_PTS + a * codec->ticks, codec->frame(a));
    Open(writer.Take(), Time(BASE_PTS) - SegmentDiff(0), INPUTSTREAM_TYPE_AUDIO);
    // Segment 1 holds only the first bytes of the PES of frame 3
    writer.WriteTables(false);
    std::vector<uint8_t> first{writer.Take()};
    writer.WriteAudioPes(BASE_PTS + 3 * codec->ticks, codec->frame(3), headerBytes);
    const std::vector<uint8_t> pes{writer.Take()};
    first.insert(first.end(), pes.begin(), pes.begin() + 188);
    AppendSegment(first, Time(BASE_PTS + 3 * codec->ticks) - SegmentDiff(1));
    writer.WriteTables(false);
    std::vector<uint8_t> second{writer.Take()};
    second.insert(second.end(), pes.begin() + 188, pes.end());
    for (int a = 4; a < 7; ++a)
      writer.WriteAudioPes(BASE_PTS + a * codec->ticks, codec->frame(a));
    const std::vector<uint8_t> rest{writer.Take()};
    second.insert(second.end(), rest.begin(), rest.end());
    AppendSegment(second, Time(BASE_PTS + 4 * codec->ticks) - SegmentDiff(2));
    std::vector<std::tuple<int, uint64_t, int64_t>> read;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
      read.emplace_back(FrameIndex(), m_reader->PTS(), m_reader->GetPTSDiff());
    std::vector<std::tuple<int, uint64_t, int64_t>> expected;
    for (int a = 0; a < 7; ++a)
      expected.emplace_back(a, Time(BASE_PTS + a * codec->ticks),
                            SegmentDiff(a < 3 ? 0 : a == 3 ? 1 : 2));
    EXPECT_EQ(read, expected);
  }
}

// Each other stream measures the time of held audio with a separate clock:
// a subtitle PTS 0.5 s after the video does not delay the release.
TEST_F(TSSampleReaderTest, HeldAudioCountsTheTimeOfEachOtherStreamApart)
{
  constexpr uint16_t SUBTITLE_PID{0x102};
  const auto tsPacket = [](uint16_t pid, uint8_t counter, const std::vector<uint8_t>& payload)
  {
    // A PES start, the payload after adaptation field stuffing
    std::vector<uint8_t> packet(188, 0xFF);
    packet[0] = 0x47;
    packet[1] = static_cast<uint8_t>(0x40 | (pid >> 8));
    packet[2] = static_cast<uint8_t>(pid);
    packet[3] = static_cast<uint8_t>(0x30 | counter);
    packet[4] = static_cast<uint8_t>(183 - payload.size());
    packet[5] = 0;
    std::copy(payload.begin(), payload.end(), packet.end() - payload.size());
    return packet;
  };
  TSWriter writer;
  writer.WriteTables(true);
  std::vector<uint8_t> data{writer.Take()};
  // The PMT with a DVB subtitle stream
  uint8_t* table{data.data() + 188};
  const uint8_t* section{table + 4 + 1 + table[4] + 1};
  const size_t length{3U + ((section[1] & 0xF) << 8) + section[2]};
  std::vector<uint8_t> pmt(section, section + length - 4);
  pmt.insert(pmt.end(), {0x06, 0xE1, 0x02, 0xF0, 0x0A, 0x59, 0x08, 'e', 'n', 'g', 0x10, 0, 1, 0, 1});
  pmt[2] += 15;
  const uint32_t crc{Crc32(pmt)};
  for (int shift = 24; shift >= 0; shift -= 8)
    pmt.push_back(static_cast<uint8_t>(crc >> shift));
  pmt.insert(pmt.begin(), 0);
  const std::vector<uint8_t> pmtPacket{tsPacket(PMT_PID, 0, pmt)};
  std::copy(pmtPacket.begin(), pmtPacket.end(), data.begin() + 188);
  for (int a = 0; a < 3; ++a)
    writer.WriteAudioFrame(a, false);
  const std::vector<uint8_t> audio{writer.Take()};
  data.insert(data.end(), audio.begin(), audio.end());
  for (uint8_t n = 0; n < 2; ++n)
  {
    const uint64_t pts{VideoPts(0) + 45000 + n * 10000};
    const std::vector<uint8_t> subtitle{tsPacket(
        SUBTITLE_PID, n,
        {0, 0, 1, 0xBD, 0, 17, 0x80, 0x80, 5, static_cast<uint8_t>(0x21 | ((pts >> 29) & 0xE)),
         static_cast<uint8_t>(pts >> 22), static_cast<uint8_t>(((pts >> 14) & 0xFE) | 1),
         static_cast<uint8_t>(pts >> 7), static_cast<uint8_t>(((pts << 1) & 0xFE) | 1), 0x20, 0,
         0x0F, 0x80, 0, 1, 0, 0, 0xFF})};
    data.insert(data.end(), subtitle.begin(), subtitle.end());
  }
  writer.WriteVideo(0, 80);
  const std::vector<uint8_t> video{writer.Take()};
  data.insert(data.end(), video.begin(), video.end());
  Open(data, Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
  m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
  m_reader->AddStreamType(INPUTSTREAM_TYPE_SUBTITLE, 3);
  int videoFrames{0};
  std::optional<int> firstAudio;
  bool started{false};
  for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
       result = m_reader->ReadSample())
  {
    if (m_reader->GetStreamId() == VIDEO_ID)
      ++videoFrames;
    else if (m_reader->GetStreamId() == AUDIO_ID && !firstAudio)
      firstAudio = videoFrames;
  }
  // The DTS of video frame 24 is more than 1 s after the DTS of frame 0
  EXPECT_EQ(firstAudio, 24);
}

// The reader keeps the PTS of a segment while a frame of that segment can
// still come: a held audio frame, or a frame of a PES in the buffers. The
// reader drops the other segments.
TEST_F(TSSampleReaderTest, TheReaderKeepsOnlyTheSegmentsOfFramesThatCanStillCome)
{
  TSWriter writer;
  for (int s = 0; s < 40; ++s)
  {
    writer.WriteTables(true);
    if (s == 0)
    {
      for (int a = 0; a < 3; ++a)
        writer.WriteAudioFrame(a, false);
    }
    writer.WriteVideo(s * 5, 5);
    const uint64_t start{Time(VideoPts(s * 5)) - DIFF};
    if (s == 0)
      Open(writer.Take(), start, INPUTSTREAM_TYPE_VIDEO);
    else
      AppendSegment(writer.Take(), start);
  }
  m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
  // The held audio of segment 0 waits up to video frame 24, in segment 4
  for (int read = 1; read < 20; ++read)
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  EXPECT_TRUE(m_reader->GetSegmentStart(0).has_value());
  for (uint32_t segment = 1; segment < 3; ++segment)
    EXPECT_FALSE(m_reader->GetSegmentStart(segment).has_value()) << segment;
  while (m_reader->GetStreamId() != AUDIO_ID)
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  for (int read = 0; read < 20; ++read)
    ASSERT_EQ(m_reader->ReadSample(), AP4_SUCCESS);
  std::vector<uint32_t> kept;
  for (uint32_t segment = 0; segment < 40; ++segment)
  {
    if (m_reader->GetSegmentStart(segment))
      kept.push_back(segment);
  }
  // The last audio PES of segment 0 waits in the buffer for the next audio
  // PES, the video PES of segment 7 for the next video PES
  EXPECT_EQ(kept, (std::vector<uint32_t>{0, 7, 8}));
}

// A PES can span more segments than a reader keeps for the buffers. The frame
// of that PES takes the PTS diff of the segment where the PES starts.
TEST_F(TSSampleReaderTest, AFrameOfAPesAcrossManySegmentsHasTheDiffOfItsSegment)
{
  TSWriter writer{H264, EAC3_71};
  writer.WriteTables(false);
  for (int unit = 0; unit < 3; ++unit)
    writer.WriteAudioPes(BASE_PTS + unit * EAC3_TICKS, Eac3AccessUnit(unit));
  Open(writer.Take(), Time(BASE_PTS) - SegmentDiff(0), INPUTSTREAM_TYPE_AUDIO);
  // An access unit of 4224 bytes in 23 TS packets, each in a separate segment
  std::vector<uint8_t> unit{Eac3Frame(0, 3, 3)};
  const std::vector<uint8_t> dependent{Eac3Frame(1, 3, 3, 2048)};
  unit.insert(unit.end(), dependent.begin(), dependent.end());
  writer.WriteAudioPes(BASE_PTS + 3 * EAC3_TICKS, unit);
  const std::vector<uint8_t> pes{writer.Take()};
  int segment{1};
  for (size_t pos = 0; pos < pes.size(); pos += 188, ++segment)
  {
    writer.WriteTables(false);
    std::vector<uint8_t> data{writer.Take()};
    data.insert(data.end(), pes.begin() + pos, pes.begin() + pos + 188);
    if (pos + 188 == pes.size())
    {
      for (int next = 4; next < 7; ++next)
        writer.WriteAudioPes(BASE_PTS + next * EAC3_TICKS, Eac3AccessUnit(next));
      const std::vector<uint8_t> units{writer.Take()};
      data.insert(data.end(), units.begin(), units.end());
    }
    AppendSegment(data, Time(BASE_PTS + 3 * EAC3_TICKS) + (segment - 1) * 1000 - SegmentDiff(segment));
  }
  std::vector<std::tuple<int, AP4_Size, uint64_t, int64_t>> read;
  bool started{false};
  for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
       result = m_reader->ReadSample())
    read.emplace_back(FrameIndex(), m_reader->GetSampleDataSize(), m_reader->PTS(),
                      m_reader->GetPTSDiff());
  ASSERT_EQ(read.size(), 7U);
  EXPECT_EQ(read[3], std::make_tuple(3, AP4_Size{4224}, Time(BASE_PTS + 3 * EAC3_TICKS),
                                     SegmentDiff(1)));
}

// A seek drops the audio frames that came with the first PTS and still wait
// to be read
TEST_F(TSSampleReaderTest, AudioReleasedBeforeASeekDoesNotComeAfterTheSeek)
{
  TSWriter writer;
  writer.WriteTables(false);
  for (int a = 0; a < 30; ++a)
    writer.WriteAudioFrame(a, a % 6 == 3);
  Open(writer.Take(), Time(AudioPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
  ASSERT_EQ(FrameIndex(), 0);
  ASSERT_TRUE(m_reader->TimeSeek(Time(AudioPts(10)), true));
  std::vector<int> read{FrameIndex()};
  while (read.size() < 3 && AP4_SUCCEEDED(m_reader->ReadSample()))
    read.push_back(FrameIndex());
  EXPECT_EQ(read, (std::vector<int>{10, 11, 12}));
}

// The PES of an E-AC-3 access unit can continue in the next segment, after
// the start of the dependent header. A seek to that access unit, or to a
// later access unit, returns the whole access units. The last segment holds
// only one access unit in a PES without a PTS, which comes at the end of the
// stream and takes the PTS diff of that segment.
TEST_F(TSSampleReaderTest, Eac3AccessUnitsAcrossSegmentsAreWhole)
{
  const auto eac3Pts = [](int unit) { return BASE_PTS + unit * EAC3_TICKS; };
  for (const size_t dependentBytes : {size_t{0}, size_t{3}, size_t{6}})
  for (const std::optional<int> target : {std::optional<int>{}, std::optional<int>{5},
                                          std::optional<int>{7}})
  {
    SCOPED_TRACE(testing::Message() << dependentBytes << " bytes of the dependent frame, "
                                    << (target ? "seek to " + std::to_string(*target)
                                               : std::string{"no seek"}));
    TSWriter writer{H264, EAC3_71};
    writer.WriteTables(false);
    for (int unit = 0; unit < 5; ++unit)
      writer.WriteAudioPes(eac3Pts(unit), Eac3AccessUnit(unit));
    // The PES header has 14 bytes
    writer.WriteAudioPes(eac3Pts(5), Eac3AccessUnit(5), 14 + 128 + dependentBytes);
    std::vector<uint8_t> first{writer.Take()};
    const std::vector<uint8_t> rest{first.end() - 188, first.end()};
    first.erase(first.end() - 188, first.end());
    writer.WriteTables(false);
    std::vector<uint8_t> second{writer.Take()};
    second.insert(second.end(), rest.begin(), rest.end());
    for (int unit = 6; unit < 9; ++unit)
      writer.WriteAudioPes(eac3Pts(unit), Eac3AccessUnit(unit));
    const std::vector<uint8_t> units{writer.Take()};
    second.insert(second.end(), units.begin(), units.end());
    writer.WriteTables(false);
    writer.WriteAudioPes(std::nullopt, Eac3AccessUnit(9));
    Open(first, Time(eac3Pts(0)) - SegmentDiff(0), INPUTSTREAM_TYPE_AUDIO);
    AppendSegment(second, Time(eac3Pts(6)) - SegmentDiff(1));
    AppendSegment(writer.Take(), Time(eac3Pts(9)) - SegmentDiff(2));
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    if (target)
    {
      m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
      m_segments->m_segmentReadPos = 0;
      m_reader->Reset(false);
      ASSERT_TRUE(m_reader->TimeSeek(Time(eac3Pts(*target)), true));
    }
    std::vector<std::tuple<int, AP4_Size, uint64_t, int64_t>> read;
    do
      read.emplace_back(FrameIndex(), m_reader->GetSampleDataSize(), m_reader->PTS(),
                        m_reader->GetPTSDiff());
    while (AP4_SUCCEEDED(m_reader->ReadSample()));
    std::vector<std::tuple<int, AP4_Size, uint64_t, int64_t>> expected;
    for (int unit = target.value_or(0); unit < 10; ++unit)
      expected.emplace_back(unit, 256, Time(eac3Pts(unit)), SegmentDiff(unit < 6 ? 0 : unit < 9 ? 1 : 2));
    EXPECT_EQ(read, expected);
  }
}

// A live stream that waits for the next segment keeps the audio frames that
// wait for a PTS
TEST_F(TSSampleReaderTest, AudioWithoutAPtsWaitsForTheNextLiveSegment)
{
  TSWriter writer;
  std::vector<uint8_t> segments[2];
  for (int s = 0; s < 2; ++s)
  {
    writer.WriteTables(false);
    for (int a = 6 * s; a < 6 * s + 6; ++a)
      writer.WriteAudioFrame(a, a == 6);
    segments[s] = writer.Take();
  }
  Open(segments[0], Time(AudioPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
  m_segments->m_waitingForSegment = true;
  bool started{false};
  EXPECT_EQ(m_reader->Start(started), AP4_ERROR_EOS);
  EXPECT_FALSE(m_reader->EOS());
  EXPECT_EQ(m_reader->ReadSample(), AP4_ERROR_EOS);
  EXPECT_FALSE(m_reader->EOS());
  AppendSegment(segments[1], Time(AudioPts(6)) - DIFF);
  m_segments->m_waitingForSegment = false;
  std::vector<std::pair<int, uint64_t>> read;
  while (AP4_SUCCEEDED(m_reader->ReadSample()))
    read.emplace_back(FrameIndex(), m_reader->PTS());
  std::vector<std::pair<int, uint64_t>> expected;
  for (int a = 0; a < 12; ++a)
    expected.emplace_back(a, Time(AudioPts(a)));
  EXPECT_EQ(read, expected);
}

// The PTS diff of a segment comes from the PTS of the first frame of the
// segment, also when the first audio PES of the segment has no PTS. The video
// sets the diff when the video is enabled. A seek uses the diff to find the
// frame of a time.
TEST_F(TSSampleReaderTest, PtsDiffOfAudioWithoutPtsComesFromTheFirstFrame)
{
  for (const bool video : {false, true})
  for (const int firstPts : {0, 3})
  {
    SCOPED_TRACE(testing::Message() << (video ? "video and audio" : "audio")
                                    << ", PTS in audio PES " << firstPts << " + 6n");
    TSWriter writer;
    std::vector<uint8_t> segments[2];
    for (int s = 0; s < 2; ++s)
    {
      writer.WriteTables(video);
      for (int v = 5 * s; v < 5 * s + 5; ++v)
      {
        if (video)
          writer.WriteVideo(v, 1, 5);
        for (int a = 2 * v; a < 2 * v + 2; ++a)
          writer.WriteAudioFrame(a, a % 6 == firstPts);
      }
      segments[s] = writer.Take();
    }
    // The video frame v starts with the audio frame 2v
    Open(segments[0], Time(AudioPts(0)) - SegmentDiff(0),
         video ? INPUTSTREAM_TYPE_VIDEO : INPUTSTREAM_TYPE_AUDIO);
    AppendSegment(segments[1], Time(AudioPts(10)) - SegmentDiff(1));
    if (video)
      m_reader->AddStreamType(INPUTSTREAM_TYPE_AUDIO, AUDIO_ID);
    std::vector<std::pair<uint64_t, int64_t>> read;
    std::vector<std::pair<uint64_t, int64_t>> expected;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
    {
      read.emplace_back(m_reader->PTS(), m_reader->GetPTSDiff());
      expected.emplace_back(m_reader->PTS(),
                            SegmentDiff(m_reader->PTS() >= Time(AudioPts(10)) ? 1 : 0));
    }
    EXPECT_EQ(read.size(), video ? 30 : 20);
    EXPECT_EQ(read, expected);
  }

  {
    // A video frame in a PES without a PTS has the PTS of the PES before, here
    // of the segment before. The first video PES with a PTS sets the diff.
    TSWriter writer;
    writer.WriteTables(true);
    writer.WriteVideo(0, 5, 5);
    const std::vector<uint8_t> first{writer.Take()};
    writer.WriteTables(true);
    writer.WriteVideoFrameWithoutPts(5, H264.keyFrame);
    writer.WriteVideo(6, 4, 5);
    Open(first, Time(VideoPts(0)) - SegmentDiff(0), INPUTSTREAM_TYPE_VIDEO);
    AppendSegment(writer.Take(), Time(VideoPts(6)) - SegmentDiff(1));
    std::vector<std::pair<uint64_t, int64_t>> read;
    bool started{false};
    for (AP4_Result result{m_reader->Start(started)}; AP4_SUCCEEDED(result);
         result = m_reader->ReadSample())
      read.emplace_back(m_reader->PTS(), m_reader->GetPTSDiff());
    std::vector<std::pair<uint64_t, int64_t>> expected;
    for (int v = 0; v < 10; ++v)
      expected.emplace_back(Time(VideoPts(v == 5 ? 4 : v)), SegmentDiff(v < 5 ? 0 : 1));
    EXPECT_EQ(read, expected);
  }

  // A seek with the diff of the first frame lands on the frame of the time
  TSWriter writer;
  writer.WriteTables(false);
  for (int a = 0; a < 30; ++a)
    writer.WriteAudioFrame(a, a % 6 == 3);
  Open(writer.Take(), Time(AudioPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
  ASSERT_EQ(m_reader->GetPTSDiff(), DIFF);
  m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
  m_segments->m_segmentReadPos = 0;
  m_reader->Reset(false);
  ASSERT_TRUE(m_reader->TimeSeek(Time(AudioPts(6)) - DIFF + m_reader->GetPTSDiff(), true));
  EXPECT_EQ(FrameIndex(), 6);
}

// A PTS has 33 bits, and the PTS after 0x1FFFFFFFF is 0. The audio frames
// before a PTS, and the frames after a PTS in the same PES, take the PTS that
// a PES of their own would carry, also across that wrap.
TEST_F(TSSampleReaderTest, AudioFramePtsWrapsAfter33Bits)
{
  constexpr uint64_t WRAP{uint64_t{1} << 33};
  // The wrap comes after the frames 0 and 1
  constexpr uint64_t SHIFT{WRAP - 2 * AUDIO_TICKS - BASE_PTS};
  const auto wrappedPts = [](int frame) { return (AudioPts(frame) + SHIFT) % WRAP; };
  // Moves each PES PTS of the stream by SHIFT
  const auto shiftPts = [](std::vector<uint8_t> data)
  {
    for (size_t packet = 0; packet < data.size(); packet += 188)
    {
      uint8_t* ts{&data[packet]};
      if (!(ts[1] & 0x40) || (((ts[1] & 0x1F) << 8) | ts[2]) != AUDIO_PID)
        continue;
      uint8_t* pes{ts + 4 + ((ts[3] & 0x20) ? 1 + ts[4] : 0)};
      if (!(pes[7] & 0x80))
        continue;
      uint8_t* field{pes + 9};
      uint64_t pts{(uint64_t{field[0] & 0x0Eu} << 29) | (uint64_t{field[1]} << 22) |
                   (uint64_t{field[2] & 0xFEu} << 14) | (uint64_t{field[3]} << 7) |
                   (field[4] >> 1)};
      pts = (pts + SHIFT) % WRAP;
      field[0] = static_cast<uint8_t>(0x21 | ((pts >> 29) & 0x0E));
      field[1] = static_cast<uint8_t>(pts >> 22);
      field[2] = static_cast<uint8_t>(((pts >> 14) & 0xFE) | 1);
      field[3] = static_cast<uint8_t>(pts >> 7);
      field[4] = static_cast<uint8_t>(((pts << 1) & 0xFE) | 1);
    }
    return data;
  };
  for (const int pesFrames : {1, 6})
  for (const bool seek : {false, true})
  {
    SCOPED_TRACE(testing::Message() << pesFrames << " frames per PES, "
                                    << (seek ? "seek to frame 1" : "start"));
    TSWriter writer;
    writer.WriteTables(false);
    if (pesFrames == 1)
    {
      // The first PTS comes after the wrap
      for (int a = 0; a < 30; ++a)
        writer.WriteAudioFrame(a, a % 6 == 3);
    }
    else
      writer.WriteAudio(AudioPts(0), 0, 30, pesFrames, false);
    Open(shiftPts(writer.Take()), Time(wrappedPts(0)) - DIFF, INPUTSTREAM_TYPE_AUDIO);
    bool started{false};
    ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
    if (seek)
    {
      m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
      m_segments->m_segmentReadPos = 0;
      m_reader->Reset(false);
      ASSERT_TRUE(m_reader->TimeSeek(Time(wrappedPts(1)), true));
    }
    std::vector<std::pair<int, uint64_t>> read;
    do
    {
      read.emplace_back(FrameIndex(), m_reader->PTS());
      EXPECT_EQ(m_reader->GetPTSDiff(), DIFF) << FrameIndex();
    } while (read.size() < 6 && AP4_SUCCEEDED(m_reader->ReadSample()));
    std::vector<std::pair<int, uint64_t>> expected;
    for (int a = seek ? 1 : 0; a < (seek ? 7 : 6); ++a)
      expected.emplace_back(a, Time(wrappedPts(a)));
    EXPECT_EQ(read, expected);
  }
}

// A seek can land on a key frame whose PES has no PTS. The read after the
// seek returns that frame with the PTS that the seek found, then the next frames.
TEST_F(TSSampleReaderTest, ASeekToAKeyFrameInAPesWithoutPtsReturnsTheFrame)
{
  TSWriter writer;
  writer.WriteTables(true);
  writer.WriteVideo(0, 10, 5);
  writer.WriteVideoFrameWithoutPts(10, H264.keyFrame);
  writer.WriteVideo(11, 9, 5);
  Open(writer.Take(), Time(VideoPts(0)) - DIFF, INPUTSTREAM_TYPE_VIDEO);
  bool started{false};
  ASSERT_EQ(m_reader->Start(started), AP4_SUCCESS);
  m_segments->m_absolutePosition -= m_segments->m_segmentReadPos;
  m_segments->m_segmentReadPos = 0;
  m_reader->Reset(false);
  ASSERT_TRUE(m_reader->TimeSeek(Time(VideoPts(10)), true));
  // The key frame has the PTS of the PES before, as in the read without a seek
  std::vector<uint64_t> pts{m_reader->PTS()};
  const AP4_Size keyFrameSize{m_reader->GetSampleDataSize()};
  while (pts.size() < 4 && AP4_SUCCEEDED(m_reader->ReadSample()))
    pts.push_back(m_reader->PTS());
  EXPECT_EQ(pts, (std::vector<uint64_t>{Time(VideoPts(9)), Time(VideoPts(11)),
                                        Time(VideoPts(12)), Time(VideoPts(13))}));
  EXPECT_GE(keyFrameSize, H264.keyFrame.size());
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
