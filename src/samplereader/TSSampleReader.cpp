/*
 *  Copyright (C) 2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TSSampleReader.h"

#include "AdaptiveByteStream.h"

CTSSampleReader::CTSSampleReader(AP4_ByteStream* input,
                               INPUTSTREAM_TYPE type,
                               AP4_UI32 streamId,
                               uint32_t requiredMask)
  : TSReader{input, requiredMask},
    m_adByteStream{dynamic_cast<CAdaptiveByteStream*>(input)},
    m_typeMask{1U << type}
{
  m_typeMap[INPUTSTREAM_TYPE_NONE] = streamId;
  m_typeMap[type] = streamId;
}

bool CTSSampleReader::Initialize(SESSION::CStream* stream)
{
  return TSReader::Initialize();
}

void CTSSampleReader::AddStreamType(INPUTSTREAM_TYPE type, uint32_t sid)
{
  m_typeMap[type] = sid;
  m_typeMask |= (1 << type);
  if (m_started)
    StartStreaming(m_typeMask);
}

void CTSSampleReader::SetStreamType(INPUTSTREAM_TYPE type, uint32_t sid)
{
  m_typeMap[type] = sid;
  m_typeMask = (1 << type);
}

bool CTSSampleReader::RemoveStreamType(INPUTSTREAM_TYPE type)
{
  m_typeMask &= ~(1 << type);
  StartStreaming(m_typeMask);
  return m_typeMask == 0;
}

AP4_Result CTSSampleReader::Start(bool& bStarted)
{
  bStarted = false;
  if (m_started)
    return AP4_SUCCESS;

  if (!StartStreaming(m_typeMask))
  {
    m_eos = true;
    return AP4_ERROR_CANNOT_OPEN_FILE;
  }

  StartClocks();
  m_started = true;
  bStarted = true;
  return ReadSample();
}

AP4_Result CTSSampleReader::ReadSample()
{
  m_sample.reset();
  if (!m_samples.empty() && !GetSeekFramePts())
  {
    m_sample = std::move(m_samples.front());
    m_samples.pop_front();
    m_dts = m_sample->dts;
    m_pts = m_sample->pts;
    m_ptsDiff = m_sample->ptsDiff;
    return AP4_SUCCESS;
  }

  if (m_seekTime)
  {
    // The seek continues from the start of the next segment. The parsers keep
    // the PES that continues from the segment before.
    TSReader::Reset(false);
    if (!Seek(*m_seekTime))
      return EndOfData();
  }

  // At the end of the stream, the frames left in the buffers follow
  while (ReadPacket() ||
         (!(m_adByteStream && m_adByteStream->waitingForSegment()) && ReadEndPacket()))
  {
    if (IsBeforeSeekFrame())
      continue;

    m_dts = (GetDts() == PTS_UNSET) ? STREAM_NOPTS_VALUE : (GetDts() * 100) / 9;
    m_pts = (GetPts() == PTS_UNSET) ? STREAM_NOPTS_VALUE : (GetPts() * 100) / 9;

    if (const std::optional<SegmentStart> start{GetSegmentStart()})
      m_ptsDiff = (start->pts * 100) / 9 - start->offset;
    if (GetSeekFramePts())
    {
      m_samples.push_back({GetStreamId(), m_dts, m_pts, TSReader::GetDuration(), m_ptsDiff,
                           {GetPacketData(), GetPacketData() + GetPacketSize()}});
      continue;
    }
    return AP4_SUCCESS;
  }
  return EndOfData();
}

AP4_Result CTSSampleReader::EndOfData()
{
  if (m_adByteStream && m_adByteStream->waitingForSegment())
  {
    // The session aligns the other streams to the PTS of a video seek
    if (m_seekTime)
      m_pts = *m_seekTime;
    else if (const std::optional<uint64_t> pts{GetSeekFramePts()})
      m_pts = (*pts * 100) / 9;
  }
  else
  {
    m_eos = true;
  }
  return AP4_ERROR_EOS;
}

bool CTSSampleReader::IsSegmentEnd(uint64_t position)
{
  return m_adByteStream && m_adByteStream->IsSegmentEnd(position);
}

void CTSSampleReader::SetPTSOffset(uint64_t offset)
{
  // The demuxer emits a PES only when the next PES starts. So after a segment
  // change, the next samples are still from the previous segment, and each
  // sample takes the diff of the segment of the sample.
  MarkSegmentStart(offset);
}

void CTSSampleReader::Reset(bool bEOS)
{
  TSReader::Reset();
  m_eos = bEOS;
  m_seekTime.reset();
  m_samples.clear();
  m_sample.reset();
}

bool CTSSampleReader::TimeSeek(uint64_t pts, bool preceeding)
{
  if (!StartStreaming(m_typeMask))
    return false;

  m_seekPreceeding = preceeding;
  if (AP4_SUCCEEDED(Seek(pts) ? ReadSample() : EndOfData()))
  {
    m_started = true;
    return true;
  }
  // Without EOS, the seek waits for a live segment and ReadSample continues the seek
  return !m_eos;
}

bool CTSSampleReader::Seek(uint64_t pts)
{
  // PTS() rounds down to microseconds, so the seek time rounds up to the PTS
  // of the frame
  if (TSReader::SeekTime((pts * 9 + 99) / 100, m_seekPreceeding))
  {
    m_seekTime.reset();
    return true;
  }
  m_seekTime = pts;
  return false;
}
