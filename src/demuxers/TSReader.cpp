/*
 *  Copyright (C) 2017 peak3d (http://www.peak3d.de)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TSReader.h"

#include "mpegts/ES_AAC.h"
#include "mpegts/debug.h"
#include "utils/Utils.h"
#include "utils/log.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdlib.h>

#include <bento4/Ap4ByteStream.h>

using namespace UTILS;

namespace
{
constexpr size_t MAX_SEGMENTS{8};

// A PTS has 33 bits, and the PTS after PTS_MASK is 0
bool IsPtsBefore(uint64_t pts, uint64_t other)
{
  const uint64_t distance{(other - pts) & PTS_MASK};
  return distance != 0 && distance <= PTS_MASK / 2;
}

void DebugLog(int level, char* msg)
{
  if (msg[std::strlen(msg) - 1] == '\n')
    msg[std::strlen(msg) - 1] = '\0';

  switch (level)
  {
    case DEMUX_DBG_ERROR:
      LOG::Log(LOGERROR, msg);
      break;
    case DEMUX_DBG_WARN:
      LOG::Log(LOGWARNING, msg);
      break;
    case DEMUX_DBG_INFO:
      LOG::Log(LOGINFO, msg);
      break;
    case DEMUX_DBG_DEBUG:
      [[fallthrough]];
    case DEMUX_DBG_PARSE:
      LOG::Log(LOGDEBUG, msg);
      break;
    default:
      break;
  }
}
} // unnamed namespace

TSReader::TSReader(AP4_ByteStream* stream, uint32_t requiredMask)
  : m_stream(stream), m_requiredMask(requiredMask), m_typeMask(0)
{
  // Uncomment to debug TSDemux library
  // TSDemux::DBGAll();
  // TSDemux::SetDBGMsgCallback(DebugLog);
}

bool TSReader::Initialize()
{
  m_AVContext = new TSDemux::AVContext(this, 0, 0);
  m_AVContext->SetSegment(m_segment);
  // Get stream Information
  if (!ReadPacket(true))
  {
    delete m_AVContext;
    m_AVContext = nullptr;
    return false;
  }
  // The read of the stream information starts the streams again
  m_parsersReset = true;
  return true;
}

TSReader::~TSReader()
{
  delete m_AVContext;
  m_AVContext = nullptr;
}

bool TSReader::ReadAV(uint64_t pos, unsigned char * data, size_t len)
{
  m_stream->Seek(pos);
  return AP4_SUCCEEDED(m_stream->Read(data, static_cast<AP4_Size>(len)));
}

void TSReader::Reset(bool resetPackets)
{
  m_stream->Tell(m_startPos);
  m_AVContext->GoPosition(m_startPos, resetPackets);
  //mark invalid for Seek operations
  m_pkt.pts = PTS_UNSET;
  if (resetPackets)
  {
    m_parsersReset = true;
    m_seekFrame.reset();
    m_frameCopies.clear();
    m_seekPesFrames.clear();
  }
}

bool TSReader::StartStreaming(AP4_UI32 typeMask)
{
  m_typeMask = typeMask;
  // All streams are ON at this place
  for (auto &tsInfo : m_streamInfos)
  {
    if (!(typeMask & (1 << tsInfo.m_streamType)))
      m_AVContext->StopStreaming(tsInfo.m_stream->pid);
    else
      m_AVContext->StartStreaming(tsInfo.m_stream->pid);
    tsInfo.m_enabled = (typeMask & (1 << tsInfo.m_streamType)) != 0;
    typeMask &= ~(1 << tsInfo.m_streamType);
  }
  return typeMask == 0;
}

bool TSReader::GetInformation(kodi::addon::InputstreamInfo& info)
{
  // Keep it in sync with TSDemux::STREAM_TYPE
  static const char* STREAMTYPEMAP[19] = {
      CODEC::NAME_UNKNOWN, CODEC::NAME_MPEG1, CODEC::NAME_MPEG2,  CODEC::NAME_MPEG1,
      CODEC::NAME_MPEG2,   CODEC::NAME_AAC,   CODEC::NAME_AAC,    CODEC::NAME_AAC,
      CODEC::NAME_H264,    CODEC::NAME_HEVC,  CODEC::NAME_AC3,    CODEC::NAME_EAC3,
      CODEC::NAME_UNKNOWN, CODEC::NAME_SRT,   CODEC::NAME_MPEG4,  CODEC::NAME_VC1,
      CODEC::NAME_UNKNOWN, CODEC::NAME_DTS,   CODEC::NAME_UNKNOWN};

  bool isChanged{false};

  for (auto& tsInfo : m_streamInfos)
  {
    if (tsInfo.m_streamType != info.GetStreamType())
      continue;

    if (!tsInfo.m_changed)
      return false;
    tsInfo.m_changed = false;

    if (tsInfo.m_streamType == INPUTSTREAM_TYPE_VIDEO)
    {
      if (tsInfo.m_stream->stream_info.fps_scale != static_cast<int>(info.GetFpsScale()) ||
          tsInfo.m_stream->stream_info.fps_rate != static_cast<int>(info.GetFpsRate()) ||
          tsInfo.m_stream->stream_info.height != static_cast<int>(info.GetHeight()) ||
          tsInfo.m_stream->stream_info.width != static_cast<int>(info.GetWidth()) ||
          (tsInfo.m_stream->stream_info.aspect > 0 &&
           tsInfo.m_stream->stream_info.aspect != info.GetAspect()))
      {
        info.SetFpsRate(tsInfo.m_stream->stream_info.fps_rate);
        info.SetFpsScale(tsInfo.m_stream->stream_info.fps_scale);
        info.SetWidth(tsInfo.m_stream->stream_info.width);
        info.SetHeight(tsInfo.m_stream->stream_info.height);
        if (tsInfo.m_stream->stream_info.aspect > 0)
          info.SetAspect(tsInfo.m_stream->stream_info.aspect);

        isChanged = true;
      }
    }
    else if (tsInfo.m_streamType == INPUTSTREAM_TYPE_AUDIO)
    {
      if (tsInfo.m_stream->stream_info.language[0])
        info.SetLanguage(tsInfo.m_stream->stream_info.language);

      if (tsInfo.m_stream->stream_info.channels != static_cast<int>(info.GetChannels()) ||
          tsInfo.m_stream->stream_info.sample_rate != static_cast<int>(info.GetSampleRate()) ||
          tsInfo.m_stream->stream_info.block_align != static_cast<int>(info.GetBlockAlign()) ||
          tsInfo.m_stream->stream_info.bit_rate != static_cast<int>(info.GetBitRate()) ||
          tsInfo.m_stream->stream_info.bits_per_sample != static_cast<int>(info.GetBitsPerSample()))
      {
        info.SetChannels(tsInfo.m_stream->stream_info.channels);
        info.SetSampleRate(tsInfo.m_stream->stream_info.sample_rate);
        info.SetBlockAlign(tsInfo.m_stream->stream_info.block_align);
        info.SetBitRate(tsInfo.m_stream->stream_info.bit_rate);
        info.SetBitsPerSample(tsInfo.m_stream->stream_info.bits_per_sample);
        isChanged = true;
      }
    }

    const char* codecName = STREAMTYPEMAP[tsInfo.m_stream->stream_type];
    if (info.GetCodecName() != codecName)
    {
      info.SetCodecName(codecName);
      isChanged = true;
    }

    STREAMCODEC_PROFILE codecProfile{CodecProfileUnknown};
    if (codecName == CODEC::NAME_AAC)
    {
      int tsCodecProfile = tsInfo.m_stream->stream_info.codecProfile;
      if (tsCodecProfile == TSDemux::ES_AAC::PROFILE_MAIN)
        codecProfile = AACCodecProfileMAIN;
      else if (tsCodecProfile == TSDemux::ES_AAC::PROFILE_LC)
        codecProfile = AACCodecProfileLOW;
      else if (tsCodecProfile == TSDemux::ES_AAC::PROFILE_SSR)
        codecProfile = AACCodecProfileSSR;
      else if (tsCodecProfile == TSDemux::ES_AAC::PROFILE_LTP)
        codecProfile = AACCodecProfileLTP;
    }

    if (codecProfile != CodecProfileUnknown && info.GetCodecProfile() != codecProfile)
    {
      info.SetCodecProfile(codecProfile);
      isChanged = true;
    }

    if (!info.CompareExtraData(tsInfo.m_stream->stream_info.extra_data,
                               tsInfo.m_stream->stream_info.extra_data_size))
    {
      info.SetExtraData(tsInfo.m_stream->stream_info.extra_data,
                        tsInfo.m_stream->stream_info.extra_data_size);
      isChanged = true;
    }
    break;
  }

  return isChanged;
}

// We assume that m_startpos is the current I-Frame position
bool TSReader::SeekTime(uint64_t timeInTs, bool preceeding)
{
  // With video, the read after a seek to a segment start starts with the
  // first video frame
  std::optional<SeekFrame> startFrame;
  for (const auto& tsInfo : m_streamInfos)
  {
    if (tsInfo.m_enabled && tsInfo.m_streamType == INPUTSTREAM_TYPE_VIDEO)
      startFrame = SeekFrame{tsInfo.m_stream->pid, 0};
  }
  const bool hasVideo{startFrame.has_value()};
  uint64_t lastRecovery(static_cast<uint64_t>(m_startPos));
  std::optional<SeekFrame> seekFrame{startFrame}; // The frame at lastRecovery or later
  // The stream can read again from this position. A frame of a PES that starts
  // before is a copy, and the read after the seek starts with the copies.
  uint64_t readStart{lastRecovery};
  std::deque<FrameCopy> copies;
  bool seekFrameIsCopy{false};
  // With video, the frames of the last PES of each other stream. The PES can
  // continue in the next segment, and then the frames are copies.
  std::deque<FrameCopy> lastPesFrames;
  // A seek that continues in the next live segment
  std::deque<FrameCopy> previousPesFrames{std::move(m_seekPesFrames)};
  m_seekPesFrames.clear();
  m_frameCopies.clear();
  if (m_parsersReset)
  {
    // The scan selects frames by PTS. The frames before the first PTS of the
    // segment take the clock of that PTS.
    const uint64_t position{m_AVContext->GetPosition()};
    std::map<uint16_t, uint64_t> resume;
    for (const auto& tsInfo : m_streamInfos)
    {
      if (tsInfo.m_enabled)
        resume[tsInfo.m_stream->pid] = position;
    }
    std::vector<ClockFrame> startFrames;
    const std::map<uint16_t, TSDemux::ElementaryStream::Clock> clocks{
        ReadClocks(startFrames, resume)};
    m_AVContext->GoPosition(position, true);
    ResumeClocks(clocks);
  }
  std::vector<ClockFrame> frames; // The frames read in the segment of the seek position
  const auto keepCopy = [&](const TSDemux::ElementaryStream& stream,
                            const TSDemux::ElementaryStream::PesStart& pes)
  {
    // The PES continues, so the frames of the PES that came before are copies too
    for (auto frame{previousPesFrames.begin()}; frame != previousPesFrames.end();)
    {
      if (frame->pkt.pid == m_pkt.pid && frame->pesPosition == pes.position)
      {
        copies.push_back(std::move(*frame));
        frame = previousPesFrames.erase(frame);
      }
      else
        ++frame;
    }
    copies.push_back({m_pkt, {m_pkt.data, m_pkt.data + m_pkt.size}, GetSegmentStart(),
                      pes.position, stream.NextFramePosition()});
  };
  // The stream cannot seek back into a segment after a read of the next
  // segment. So the scan reads the frames left in the buffers at the end of a
  // segment, before the next segment.
  uint64_t segmentEnd{0}; // The end time of the frames read that set the seek position
  bool nextSegment{false};
  while (true)
  {
    m_stopAtSegmentEnd = !nextSegment;
    const bool read{ReadPacket()};
    m_stopAtSegmentEnd = false;
    if (!read)
    {
      if (nextSegment || !IsSegmentEnd(m_AVContext->GetPosition()))
      {
        // The seek can continue in the next live segment
        if (nextSegment)
          m_seekPesFrames = std::move(previousPesFrames);
        return false;
      }
      if (preceeding && timeInTs < segmentEnd)
        break;
      // The seek time is after the frames of this segment
      nextSegment = true;
      readStart = lastRecovery = m_AVContext->GetPosition();
      seekFrame = startFrame;
      seekFrameIsCopy = false;
      copies.clear();
      frames.clear();
      // A stream without frames since then, as when a live seek waits again,
      // keeps the frames of its last PES
      std::set<uint16_t> pidsWithFrames;
      for (const FrameCopy& last : lastPesFrames)
        pidsWithFrames.insert(last.pkt.pid);
      for (FrameCopy& frame : previousPesFrames)
      {
        if (!pidsWithFrames.count(frame.pkt.pid))
          lastPesFrames.push_back(std::move(frame));
      }
      previousPesFrames = std::move(lastPesFrames);
      lastPesFrames.clear();
      continue;
    }
    nextSegment = false;
    // The demuxer emits a frame only after a later PES starts. The frame
    // starts in the PES where the frame data starts.
    const TSDemux::ElementaryStream* stream{m_AVContext->GetStream(m_pkt.pid)};
    const TSDemux::ElementaryStream::PesStart* pes{stream ? stream->GetPesStart(m_pkt.data)
                                                         : nullptr};
    if (!pes)
      continue;
    frames.push_back({m_pkt, *pes});
    const bool isCopy{pes->position < readStart};
    if (isCopy)
      keepCopy(*stream, *pes);
    else if (hasVideo && GetStreamType() != INPUTSTREAM_TYPE_VIDEO)
    {
      const uint16_t pid{m_pkt.pid};
      lastPesFrames.erase(std::remove_if(lastPesFrames.begin(), lastPesFrames.end(),
                                         [pid, pes](const FrameCopy& frame) {
                                           return frame.pkt.pid == pid &&
                                                  frame.pesPosition != pes->position;
                                         }),
                          lastPesFrames.end());
      lastPesFrames.push_back({m_pkt, {m_pkt.data, m_pkt.data + m_pkt.size}, GetSegmentStart(),
                               pes->position, stream->NextFramePosition()});
    }
    if (m_pkt.pts == PTS_UNSET)
      continue;
    const uint64_t pts{static_cast<uint64_t>(m_pkt.pts)};
    // With video, the video frames set the seek position
    if (hasVideo && GetStreamType() != INPUTSTREAM_TYPE_VIDEO)
      continue;
    segmentEnd = std::max(segmentEnd, pts + m_pkt.duration);
    if (!hasVideo || m_pkt.recoveryPoint || pes->position == m_startPos)
    {
      // A preceeding seek lands on a frame at or before the seek time
      if (preceeding && pts > timeInTs)
        break;
      lastRecovery = isCopy ? readStart : pes->position;
      // An audio PES holds more than one frame
      seekFrame = SeekFrame{m_pkt.pid, pts};
      seekFrameIsCopy = isCopy;
      if (pts >= timeInTs)
        break;
    }
    // No later key frame is at or before the seek time
    else if (preceeding && pts >= timeInTs)
      break;
  }
  std::deque<FrameCopy> frameCopies;
  if (lastRecovery == readStart)
  {
    // A stream emits the rest of a PES before readStart when the next PES of
    // the stream starts. The read stays in the segment of readStart.
    std::vector<uint16_t> pending;
    for (const auto& tsInfo : m_streamInfos)
    {
      if (tsInfo.m_enabled)
        pending.push_back(tsInfo.m_stream->pid);
    }
    m_stopAtSegmentEnd = true;
    while (!pending.empty() && ReadPacket())
    {
      const TSDemux::ElementaryStream* stream{m_AVContext->GetStream(m_pkt.pid)};
      const TSDemux::ElementaryStream::PesStart* pes{stream ? stream->GetPesStart(m_pkt.data)
                                                           : nullptr};
      if (!pes)
        continue;
      frames.push_back({m_pkt, *pes});
      if (pes->position < readStart)
        keepCopy(*stream, *pes);
      else
        pending.erase(std::remove(pending.begin(), pending.end(), m_pkt.pid), pending.end());
    }
    m_stopAtSegmentEnd = false;
    // Before the first video frame of a segment, only the frames of other
    // streams follow
    for (FrameCopy& copy : copies)
    {
      if (seekFrameIsCopy || !seekFrame || copy.pkt.pid != seekFrame->pid)
        frameCopies.push_back(std::move(copy));
    }
  }
  // The data of the last copied frame of a stream can continue after
  // lastRecovery, also in a PES of its own. The parser does not see a copied
  // key frame, so the frames after the copy continue the decode of that key frame.
  std::map<uint16_t, uint64_t> nextPositions;
  for (const FrameCopy& copy : frameCopies)
    nextPositions[copy.pkt.pid] = copy.nextPosition;
  // Each stream continues with the first frame of a PES at its resume position
  // or later. A PES with a PTS sets the clock of the stream.
  std::map<uint16_t, uint64_t> resume;
  for (const auto& tsInfo : m_streamInfos)
  {
    if (tsInfo.m_enabled)
      resume[tsInfo.m_stream->pid] = lastRecovery;
  }
  // A copy without a PES of the next frame continues at lastRecovery
  for (const auto& [pid, position] : nextPositions)
  {
    if (position)
      resume[pid] = position;
  }
  const std::map<uint16_t, TSDemux::ElementaryStream::Clock> clocks{ReadClocks(frames, resume)};
  m_frameCopies = std::move(frameCopies);
  m_AVContext->GoPosition(lastRecovery, true);
  for (const auto& [pid, position] : nextPositions)
  {
    if (TSDemux::ElementaryStream* stream{m_AVContext->GetStream(pid)})
      stream->ContinueAfterCopy(position, seekFrameIsCopy && pid == seekFrame->pid);
  }
  ResumeClocks(clocks);
  m_seekFrame = seekFrame;

  return true;
}

bool TSReader::ReadPacket(bool scanStreamInfo)
{
  return PacketRead(ReadNextPacket(scanStreamInfo));
}

bool TSReader::ReadEndPacket()
{
  return PacketRead(ReadNextEndPacket(true));
}

bool TSReader::PacketRead(bool read)
{
  if (!read)
    return false;
  m_parsersReset = false;
  // The first audio PES of a segment can have no PTS, the first frame has the
  // PTS of the stream in the segment
  if (m_pkt.pts != PTS_UNSET && GetStreamType() == INPUTSTREAM_TYPE_AUDIO)
  {
    if (const std::optional<uint32_t> segment{GetPacketSegment()})
      UpdateSegmentFramePts(*segment, m_pkt.pid, m_pkt.pts);
  }
  return true;
}

bool TSReader::ReadNextPacket(bool scanStreamInfo)
{
  if (!m_AVContext)
    return false;

  if (!m_frameCopies.empty())
  {
    m_frameCopy = std::move(m_frameCopies.front());
    m_frameCopies.pop_front();
    m_pkt = m_frameCopy->pkt;
    m_pkt.data = m_frameCopy->data.data();
    return true;
  }

  bool ret(false);

  if (GetPacket())
    return true;

  while (!ret)
  {
    if (m_stopAtSegmentEnd && IsSegmentEnd(m_AVContext->GetPosition()))
      return ReadNextEndPacket(false);

    int errorCode;
    if ((errorCode = m_AVContext->TSResync()) != TSDemux::AVCONTEXT_CONTINUE)
    {
      if (errorCode != TSDemux::AVCONTEXT_IO_ERROR)
        return false;
      //Lets make one retry with the new segment
      Reset(false);
      if (m_AVContext->TSResync() != TSDemux::AVCONTEXT_CONTINUE)
        return false;
    }

    int status = m_AVContext->ProcessTSPacket();

    while (GetPacket())
    {
      if (scanStreamInfo)
      {
        if (m_pkt.streamChange)
        {
          if (HandleStreamChange(m_pkt.pid))
          {
            m_AVContext->GoPosition(m_startPos, true);
            StartStreaming(m_typeMask);
            return true;
          }
        }
      }
      else
      {
        if (m_pkt.streamChange)
          HandleStreamChange(m_pkt.pid);
        return true;
      }
    }

    if (m_AVContext->HasPIDPayload())
    {
      TSDemux::ElementaryStream* stream{m_AVContext->GetPIDStream()};
      const uint32_t pesPtsCount{stream ? stream->pes_pts_count : 0};
      status = m_AVContext->ProcessTSPayload();
      // Take the PTS before the parser of the stream advances the PTS frame by frame
      if (stream && stream->pes_pts_count != pesPtsCount)
        UpdateSegmentPts(stream);
      if (status == TSDemux::AVCONTEXT_PROGRAM_CHANGE)
      {
        if (HandleProgramChange())
        {
          if (scanStreamInfo)
            ret = true;
        }
        else
        {
          scanStreamInfo = true;
          m_startPos = m_AVContext->GetNextPosition();
        }
      }
    }

    if (status == TSDemux::AVCONTEXT_TS_ERROR)
      m_AVContext->Shift();
    else
      m_AVContext->GoNext();
  }
  return true;
}

std::map<uint16_t, TSDemux::ElementaryStream::Clock> TSReader::ReadClocks(
    std::vector<ClockFrame>& frames, const std::map<uint16_t, uint64_t>& resume)
{
  // The clock of a stream at the first frame of a PES at the resume position
  // or later. The frames of a PES without a PTS follow the frames before, so
  // without a clock at that frame, the next PTS gives the clock.
  std::map<uint16_t, TSDemux::ElementaryStream::Clock> clocks;
  const auto findClocks = [&]()
  {
    bool complete{true};
    for (const auto& [pid, position] : resume)
    {
      if (position == UINT64_MAX || clocks.count(pid))
        continue;
      const auto first{std::find_if(frames.begin(), frames.end(),
                                    [pid = pid, position = position](const ClockFrame& frame)
                                    {
                                      return frame.pkt.pid == pid &&
                                             frame.pes.position >= position;
                                    })};
      if (first == frames.end())
      {
        complete = false;
        continue;
      }
      if (first->pes.timestamped || first->pkt.pts != PTS_UNSET)
      {
        clocks[pid] = {first->pes.pts, first->pes.dts, first->pkt.pts};
        continue;
      }
      int64_t duration{0};
      bool found{false};
      for (auto frame{first}; frame != frames.end(); ++frame)
      {
        if (frame->pkt.pid != pid)
          continue;
        if (frame->pkt.pts != PTS_UNSET)
        {
          clocks[pid] = {first->pes.pts, first->pes.dts, (frame->pkt.pts - duration) & PTS_MASK};
          found = true;
          break;
        }
        duration += frame->pkt.duration;
      }
      complete = complete && found;
    }
    return complete;
  };
  m_stopAtSegmentEnd = true;
  while (!findClocks() && ReadPacket())
  {
    const TSDemux::ElementaryStream* stream{m_AVContext->GetStream(m_pkt.pid)};
    if (const TSDemux::ElementaryStream::PesStart* pes{stream ? stream->GetPesStart(m_pkt.data)
                                                              : nullptr})
      frames.push_back({m_pkt, *pes});
  }
  m_stopAtSegmentEnd = false;
  return clocks;
}

void TSReader::ResumeClocks(const std::map<uint16_t, TSDemux::ElementaryStream::Clock>& clocks)
{
  for (const auto& [pid, clock] : clocks)
  {
    if (TSDemux::ElementaryStream* stream{m_AVContext->GetStream(pid)})
      stream->ResumeClock(clock);
  }
}

bool TSReader::ReadNextEndPacket(bool endOfStream)
{
  for (auto& tsInfo : m_streamInfos)
  {
    if (tsInfo.m_enabled && tsInfo.m_stream->GetEndPacket(&m_pkt, endOfStream))
    {
      if (m_pkt.streamChange)
        HandleStreamChange(m_pkt.pid);
      return true;
    }
  }
  return false;
}

bool TSReader::GetPacket()
{
  if (m_AVContext->HasPIDStreamData())
  {
    TSDemux::ElementaryStream* es = m_AVContext->GetPIDStream();
    if (!es)
      return false;

    return es->GetStreamPacket(&m_pkt);
  }
  return false;
}

bool TSReader::HasVideo() const
{
  for (const auto& tsInfo : m_streamInfos)
  {
    if (tsInfo.m_enabled && tsInfo.m_streamType == INPUTSTREAM_TYPE_VIDEO)
      return true;
  }
  return false;
}

void TSReader::MarkSegmentStart(uint64_t offset, std::optional<uint32_t> oldestUsed)
{
  if (!m_segments.back().offset)
  {
    m_segments.back().offset = offset;
    return;
  }
  m_segments.push_back({++m_segment, offset, {}});
  while (m_segments.size() > MAX_SEGMENTS &&
         (!oldestUsed || m_segments.front().number < *oldestUsed))
    m_segments.pop_front();
  // The PES packets that start from now on are in the new segment
  if (m_AVContext)
    m_AVContext->SetSegment(m_segment);
}

std::optional<TSReader::SegmentStart> TSReader::GetSegmentStart() const
{
  if (m_frameCopy && m_pkt.data == m_frameCopy->data.data())
    return m_frameCopy->segmentStart;
  if (const std::optional<uint32_t> segment{GetPacketSegment()})
    return GetSegmentStart(*segment);
  return {};
}

std::optional<uint32_t> TSReader::GetPacketSegment() const
{
  if (m_frameCopy && m_pkt.data == m_frameCopy->data.data())
    return {};
  const TSDemux::ElementaryStream* stream{m_AVContext->GetStream(m_pkt.pid)};
  const TSDemux::ElementaryStream::PesStart* pes{stream ? stream->GetPesStart(m_pkt.data)
                                                       : nullptr};
  if (!pes)
    return {};
  return pes->segment;
}

std::optional<TSReader::SegmentStart> TSReader::GetSegmentStart(uint32_t number) const
{
  const auto segment{std::find_if(m_segments.begin(), m_segments.end(),
                                  [number](const Segment& s) { return s.number == number; })};
  if (segment == m_segments.end() || !segment->offset)
    return {};
  // With video, the audio PES can start before the video PES in some segments
  const bool hasVideo{HasVideo()};
  for (const auto& [pid, pts] : segment->pts)
  {
    for (const auto& tsInfo : m_streamInfos)
    {
      if (tsInfo.m_stream->pid == pid && tsInfo.m_enabled &&
          (tsInfo.m_streamType == INPUTSTREAM_TYPE_VIDEO || !hasVideo))
        return SegmentStart{pts, *segment->offset};
    }
  }
  return {};
}

bool TSReader::IsBeforeSeekFrame()
{
  if (!m_seekFrame || m_pkt.pid != m_seekFrame->pid)
    return false;
  if (m_pkt.pts != PTS_UNSET && static_cast<uint64_t>(m_pkt.pts) < m_seekFrame->pts)
    return true;
  m_seekFrame.reset();
  return false;
}

void TSReader::UpdateSegmentPts(TSDemux::ElementaryStream* stream)
{
  // The PES belongs to the segment where the PES starts, the header can end
  // in the next segment. A seek inside the segment reads the PES headers
  // again. The read of the stream information reads the first segment before
  // the streams are enabled.
  const auto segment{std::find_if(m_segments.begin(), m_segments.end(), [stream](const Segment& s)
                                  { return s.number == stream->GetPesSegment(); })};
  if (segment != m_segments.end() &&
      std::none_of(segment->pts.begin(), segment->pts.end(),
                   [stream](const auto& pts) { return pts.first == stream->pid; }))
    segment->pts.emplace_back(stream->pid, stream->c_pts);
}

void TSReader::UpdateSegmentFramePts(uint32_t number, uint16_t pid, uint64_t pts)
{
  const auto segment{std::find_if(m_segments.begin(), m_segments.end(),
                                  [number](const Segment& s) { return s.number == number; })};
  if (segment == m_segments.end())
    return;
  const auto first{std::find_if(segment->pts.begin(), segment->pts.end(),
                                [pid](const auto& first) { return first.first == pid; })};
  if (first == segment->pts.end())
    segment->pts.emplace_back(pid, pts);
  else if (IsPtsBefore(pts, first->second))
    first->second = pts;
}

bool TSReader::HandleProgramChange()
{
  bool ret = true;
  m_streamInfos.clear();

  std::vector<TSDemux::ElementaryStream*> streams = m_AVContext->GetStreams();
  for (auto stream : streams)
  {
    m_streamInfos.push_back(TSINFO(stream));
    TSINFO &tsInfo(m_streamInfos.back());

    switch (tsInfo.m_stream->stream_type)
    {
    case TSDemux::STREAM_TYPE_VIDEO_MPEG1:
    case TSDemux::STREAM_TYPE_VIDEO_MPEG2:
    case TSDemux::STREAM_TYPE_VIDEO_H264:
    case TSDemux::STREAM_TYPE_VIDEO_HEVC:
    case TSDemux::STREAM_TYPE_VIDEO_MPEG4:
    case TSDemux::STREAM_TYPE_VIDEO_VC1:
      tsInfo.m_streamType = INPUTSTREAM_TYPE_VIDEO;
      break;
    case TSDemux::STREAM_TYPE_AUDIO_MPEG1:
    case TSDemux::STREAM_TYPE_AUDIO_MPEG2:
    case TSDemux::STREAM_TYPE_AUDIO_AAC:
    case TSDemux::STREAM_TYPE_AUDIO_AAC_ADTS:
    case TSDemux::STREAM_TYPE_AUDIO_AAC_LATM:
    case TSDemux::STREAM_TYPE_AUDIO_AC3:
    case TSDemux::STREAM_TYPE_AUDIO_EAC3:
    case TSDemux::STREAM_TYPE_AUDIO_LPCM:
    case TSDemux::STREAM_TYPE_AUDIO_DTS:
      tsInfo.m_streamType = INPUTSTREAM_TYPE_AUDIO;
      break;
    case TSDemux::STREAM_TYPE_DVB_SUBTITLE:
      tsInfo.m_streamType = INPUTSTREAM_TYPE_SUBTITLE;
      break;
    default:
      tsInfo.m_streamType = INPUTSTREAM_TYPE_NONE;
    }

    if (stream->has_stream_info)
      HandleStreamChange(stream->pid);
    else if (m_requiredMask & (1 << tsInfo.m_streamType))
      ret = false;
    else
    {
      tsInfo.m_needInfo = false;
      continue;
    }
    m_AVContext->StartStreaming(stream->pid);
  }
  return ret;
}

bool TSReader::HandleStreamChange(uint16_t pid)
{
  bool ret(true);
  for (auto &tsInfo : m_streamInfos)
  {
    if (tsInfo.m_stream->pid == pid)
    {
      tsInfo.m_needInfo = false;
      tsInfo.m_changed = true;
    }
    else if (tsInfo.m_needInfo)
      ret = false;
  }
  return ret;
}

const INPUTSTREAM_TYPE TSReader::GetStreamType() const
{
  for (const auto &tsInfo : m_streamInfos)
    if (tsInfo.m_stream && tsInfo.m_stream->pid == m_pkt.pid)
      return tsInfo.m_streamType;
  return INPUTSTREAM_TYPE_NONE;
}
