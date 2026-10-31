/*
 *  Copyright (C) 2013 Jean-Luc Barriere
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "elementaryStream.h"
#include "debug.h"

#include <cstdlib>    // for malloc free size_t
#include <cstring>    // memset memcpy memmove
#include <climits>    // for INT_MAX
#include <cerrno>

using namespace TSDemux;

ElementaryStream::ElementaryStream(uint16_t pes_pid)
  : pid(pes_pid)
  , stream_type(STREAM_TYPE_UNKNOWN)
  , c_dts(PTS_UNSET)
  , c_pts(PTS_UNSET)
  , p_dts(PTS_UNSET)
  , p_pts(PTS_UNSET)
  , pes_pts_count(0)
  , has_stream_info(false)
  , es_alloc_init(ES_INIT_BUFFER_SIZE)
  , es_buf(NULL)
  , es_alloc(0)
  , es_len(0)
  , es_consumed(0)
  , es_parsed(0)
  , es_end_of_data(false)
  , es_skip_to_pts(false)
  , es_continue_decode(false)
  , es_copy_next_position(0)
  , es_resume_position(0)
  , es_drop_pes(false)
  , es_pes_start_pending(false)
  , es_pes_start_position(0)
  , es_pes_start_segment(0)
  , es_found_frame(false)
  , es_picture_start(0)
  , es_frame_duration(0)
  , es_pts_position(UINT64_MAX)
  , es_next_pts(PTS_UNSET)
  , es_frame_valid(false)
  , es_extraDataChanged(false)
{
  memset(&stream_info, 0, sizeof(STREAM_INFO));
}

ElementaryStream::~ElementaryStream(void)
{
  if (es_buf)
  {
    DBG(DEMUX_DBG_DEBUG, "free stream buffer %.4x: allocated size was %zu\n", pid, es_alloc);
    free(es_buf);
    es_buf = NULL;
  }
}

void ElementaryStream::Reset(void)
{
  ClearBuffer();
  es_found_frame = false;
  es_frame_valid = false;
  // After a frame copy, the PES that start before the next frame belong to the copy
  const uint64_t next = std::exchange(es_copy_next_position, 0);
  es_skip_to_pts = next == UINT64_MAX;
  es_resume_position = next == UINT64_MAX ? 0 : next;
  es_drop_pes = false;
  es_pts_position = UINT64_MAX;
  es_next_pts = PTS_UNSET;
  // A PES without a PTS takes the timestamps of the PES before
  if (const std::optional<Clock> clock{std::exchange(es_resume_clock, std::nullopt)})
  {
    c_pts = clock->pts;
    c_dts = clock->dts;
    es_next_pts = clock->nextPts;
  }
}

void ElementaryStream::ClearBuffer()
{
  ShiftPesStarts(es_len);
  es_len = es_consumed = es_parsed = 0;
}

void ElementaryStream::StartPes(uint64_t position, uint32_t segment)
{
  es_drop_pes = position < es_resume_position;
  es_pes_start_pending = true;
  es_pes_start_position = position;
  es_pes_start_segment = segment;
}

const ElementaryStream::PesStart* ElementaryStream::GetPesStart(const unsigned char* data) const
{
  if (!es_buf || data < es_buf)
    return NULL;
  // The parsers also assign a frame to a PES by the position of the frame start
  const size_t offset = data - es_buf;
  for (auto it = es_pes_starts.rbegin(); it != es_pes_starts.rend(); ++it)
  {
    if (it->offset <= offset)
      return &*it;
  }
  return NULL;
}

uint64_t ElementaryStream::NextFramePosition() const
{
  if (es_skip_to_pts)
    return UINT64_MAX;
  const PesStart* pes = GetPesStart(es_buf + es_consumed);
  return pes ? pes->position : 0;
}

void ElementaryStream::ContinueAfterCopy(uint64_t nextFramePosition, bool keyFrame)
{
  es_copy_next_position = nextFramePosition;
  es_continue_decode = keyFrame;
}

void ElementaryStream::ResumeClock(const Clock& clock)
{
  es_resume_clock = clock;
}

const ElementaryStream::PesStart* ElementaryStream::StartPicture(size_t offset)
{
  es_picture_start = offset - es_consumed;
  return GetPesStart(es_buf + offset);
}

uint64_t ElementaryStream::FrameDuration(int64_t dts, uint64_t fallback)
{
  // The next frame starts at es_consumed. Without the DTS of the next frame,
  // as for the last frame of the data, the frame takes the last duration.
  const PesStart* next = GetPesStart(es_buf + es_consumed);
  if (next && next->dts != PTS_UNSET && dts != PTS_UNSET && next->dts > dts)
    es_frame_duration = next->dts - dts;
  return es_frame_duration ? es_frame_duration : fallback;
}

int64_t ElementaryStream::AudioFramePts(size_t offset, int64_t duration)
{
  // The PTS of a PES belongs to the first frame that starts in the PES. Each
  // later frame follows the frame before.
  const PesStart* pes = GetPesStart(es_buf + offset);
  if (pes && pes->timestamped && pes->position != es_pts_position)
  {
    es_pts_position = pes->position;
    es_next_pts = pes->pts;
  }
  const int64_t pts = es_next_pts;
  if (es_next_pts != PTS_UNSET)
    es_next_pts = (es_next_pts + duration) & PTS_MASK; // The PTS has 33 bits
  return pts;
}

void ElementaryStream::EndLastFrame()
{
  // The PTS of a PES belongs to the first frame that starts in the PES, and
  // the picture of a frame starts in one PES. So a later PES with a PTS starts
  // the next frame, and the next data continues that frame. A PES with a PTS
  // before the picture, as with the parameter sets, belongs to the frame.
  const size_t picture = es_consumed + es_picture_start;
  for (const PesStart& pes : es_pes_starts)
  {
    if (pes.timestamped && pes.offset > picture)
    {
      es_consumed = pes.offset;
      return;
    }
  }
  es_consumed = es_len;
  es_skip_to_pts = true;
}

bool ElementaryStream::AppendExtraData(const uint8_t* nal, size_t size)
{
  uint8_t* ed = stream_info.extra_data + stream_info.extra_data_size;
  if (stream_info.extra_data_size + 4 + size > sizeof(stream_info.extra_data))
  {
    DBG(DEMUX_DBG_INFO, "%s: stream_info.extra_data too small! %zu\n", GetStreamCodecName(),
        stream_info.extra_data_size + 4 + size);
    return false;
  }
  ed[0] = ed[1] = ed[2] = 0, ed[3] = 1;
  memcpy(ed + 4, nal, size);
  stream_info.extra_data_size += 4 + size;
  return true;
}

void ElementaryStream::ShiftPesStarts(size_t consumed)
{
  // Keep the last PES that starts at or before the new buffer start
  size_t first = 0;
  while (first + 1 < es_pes_starts.size() && es_pes_starts[first + 1].offset <= consumed)
    ++first;
  es_pes_starts.erase(es_pes_starts.begin(), es_pes_starts.begin() + first);
  for (auto& start : es_pes_starts)
    start.offset = start.offset > consumed ? start.offset - consumed : 0;
}

int ElementaryStream::Append(const unsigned char* buf, size_t len, bool new_pts)
{
  if (es_drop_pes)
  {
    es_pes_start_pending = false;
    return 0;
  }
  // A seek scan reads on after the end of a segment. The rest of a frame that
  // the end of the segment completed is not the start of the next frame.
  if (es_skip_to_pts)
  {
    if (!new_pts)
    {
      es_pes_start_pending = false;
      return 0;
    }
    es_skip_to_pts = false;
  }

  if (es_buf && es_consumed)
  {
    if (es_consumed < es_len)
    {
      memmove(es_buf, es_buf + es_consumed, es_len - es_consumed);
      es_len -= es_consumed;
      es_parsed -= es_consumed;
      ShiftPesStarts(es_consumed);

      es_consumed = 0;
    }
    else
      ClearBuffer();
  }
  if (es_len + len > es_alloc)
  {
    if (es_alloc >= ES_MAX_BUFFER_SIZE)
      return -ENOMEM;

    size_t n = (es_alloc ? (es_alloc + len) * 2 : es_alloc_init);
    if (n > ES_MAX_BUFFER_SIZE)
      n = ES_MAX_BUFFER_SIZE;

    DBG(DEMUX_DBG_DEBUG, "realloc buffer size to %zu for stream %.4x\n", n, pid);
    unsigned char* p = es_buf;
    es_buf = (unsigned char*)realloc(es_buf, n * sizeof(*es_buf));
    if (es_buf)
    {
      es_alloc = n;
    }
    else
    {
      free(p);
      es_alloc = 0;
      es_len = 0;
      return -ENOMEM;
    }
  }

  if (!es_buf)
    return -ENOMEM;

  if (es_pes_start_pending)
  {
    es_pes_start_pending = false;
    // The data of a PES without a PTS has the timestamps of the PES before
    PesStart start{es_len, es_pes_start_position, es_pes_start_segment, new_pts, c_pts, c_dts};
    if (!es_pes_starts.empty() && es_pes_starts.back().offset == es_len)
      es_pes_starts.back() = start;
    else
      es_pes_starts.push_back(start);
  }

  memcpy(es_buf + es_len, buf, len);
  es_len += len;

  return 0;
}

const char* ElementaryStream::GetStreamCodecName(STREAM_TYPE stream_type)
{
  switch (stream_type)
  {
    case STREAM_TYPE_VIDEO_MPEG1:
      return "mpeg1video";
    case STREAM_TYPE_VIDEO_MPEG2:
      return "mpeg2video";
    case STREAM_TYPE_AUDIO_MPEG1:
      return "mp1";
    case STREAM_TYPE_AUDIO_MPEG2:
      return "mp2";
    case STREAM_TYPE_AUDIO_AAC:
      return "aac";
    case STREAM_TYPE_AUDIO_AAC_ADTS:
      return "aac";
    case STREAM_TYPE_AUDIO_AAC_LATM:
      return "aac_latm";
    case STREAM_TYPE_VIDEO_H264:
      return "h264";
    case STREAM_TYPE_VIDEO_HEVC:
      return "hevc";
    case STREAM_TYPE_AUDIO_AC3:
      return "ac3";
    case STREAM_TYPE_AUDIO_EAC3:
      return "eac3";
    case STREAM_TYPE_DVB_TELETEXT:
      return "teletext";
    case STREAM_TYPE_DVB_SUBTITLE:
      return "dvbsub";
    case STREAM_TYPE_VIDEO_MPEG4:
      return "mpeg4video";
    case STREAM_TYPE_VIDEO_VC1:
      return "vc1";
    case STREAM_TYPE_AUDIO_LPCM:
      return "lpcm";
    case STREAM_TYPE_AUDIO_DTS:
      return "dts";
    case STREAM_TYPE_PRIVATE_DATA:
    default:
      return "data";
  }
}

const char* ElementaryStream::GetStreamCodecName() const
{
  return GetStreamCodecName(stream_type);
}

bool ElementaryStream::GetStreamPacket(STREAM_PKT* pkt)
{
  ResetStreamPacket(pkt);
  Parse(pkt);
  if (pkt->data)
    return true;
  return false;
}

bool ElementaryStream::GetEndPacket(STREAM_PKT* pkt)
{
  es_end_of_data = true;
  const bool ret = GetStreamPacket(pkt);
  es_end_of_data = false;
  return ret;
}

void ElementaryStream::Parse(STREAM_PKT* pkt)
{
  // No parser: pass-through
  if (es_consumed < es_len)
  {
    es_consumed = es_parsed = es_len;
    pkt->pid              = pid;
    pkt->size             = es_consumed;
    pkt->data             = es_buf;
    pkt->dts              = c_dts;
    pkt->pts              = c_pts;
    if (c_dts == PTS_UNSET || p_dts == PTS_UNSET)
      pkt->duration       = 0;
    else
      pkt->duration       = c_dts - p_dts;
    pkt->streamChange     = false;
  }
}

void ElementaryStream::ResetStreamPacket(STREAM_PKT* pkt)
{
  pkt->pid                = 0xffff;
  pkt->size               = 0;
  pkt->data               = NULL;
  pkt->dts                = PTS_UNSET;
  pkt->pts                = PTS_UNSET;
  pkt->duration           = 0;
  pkt->streamChange       = false;
  pkt->recoveryPoint      = false;
}

uint64_t ElementaryStream::Rescale(uint64_t a, uint64_t b, uint64_t c)
{
  uint64_t r = c / 2;

  if (b <= INT_MAX && c <= INT_MAX)
  {
    if (a <= INT_MAX)
      return (a * b + r) / c;
    else
      return a / c * b + (a % c * b + r) / c;
  }
  else
  {
    uint64_t a0 = a & 0xFFFFFFFF;
    uint64_t a1 = a >> 32;
    uint64_t b0 = b & 0xFFFFFFFF;
    uint64_t b1 = b >> 32;
    uint64_t t1 = a0 * b1 + a1 * b0;
    uint64_t t1a = t1 << 32;

    a0 = a0 * b0 + t1a;
    a1 = a1 * b1 + (t1 >> 32) + (a0 < t1a);
    a0 += r;
    a1 += a0 < r;

    for (int i = 63; i >= 0; i--)
    {
      a1 += a1 + ((a0 >> i) & 1);
      t1 += t1;
      if (c <= a1)
      {
        a1 -= c;
        t1++;
      }
    }
    return t1;
  }
}

bool ElementaryStream::SetVideoInformation(int FpsScale, int FpsRate, int Height, int Width, float Aspect, bool Interlaced)
{
  bool ret = false;
  if ((stream_info.fps_scale != FpsScale) ||
      (stream_info.fps_rate != FpsRate) ||
      (stream_info.height != Height) ||
      (stream_info.width != Width) ||
      (stream_info.aspect != Aspect) ||
      (stream_info.interlaced != Interlaced))
    ret = true;

  stream_info.fps_scale       = FpsScale;
  stream_info.fps_rate        = FpsRate;
  stream_info.height          = Height;
  stream_info.width           = Width;
  stream_info.aspect          = Aspect;
  stream_info.interlaced      = Interlaced;

  has_stream_info = true;
  return ret;
}

bool ElementaryStream::SetAudioInformation(int Channels,
                                           int SampleRate,
                                           int BitRate,
                                           int BitsPerSample,
                                           int BlockAlign,
                                           int codecProfile /* = 0 */)
{
  bool ret = false;
  if ((stream_info.channels != Channels) ||
      (stream_info.sample_rate != SampleRate) ||
      (stream_info.block_align != BlockAlign) ||
      (stream_info.bit_rate != BitRate) ||
      (stream_info.bits_per_sample != BitsPerSample) ||
      (stream_info.codecProfile != codecProfile))
    ret = true;

  stream_info.channels          = Channels;
  stream_info.sample_rate       = SampleRate;
  stream_info.block_align       = BlockAlign;
  stream_info.bit_rate          = BitRate;
  stream_info.bits_per_sample   = BitsPerSample;
  stream_info.codecProfile = codecProfile;

  has_stream_info = true;
  return ret;
}
