/*
 *  Copyright (C) 2017 peak3d (http://www.peak3d.de)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "mpegts/tsDemuxer.h"

#include <deque>
#include <optional>
#include <stdint.h>
#include <utility>
#include <vector>

#include <bento4/Ap4Types.h>
#include <kodi/addon-instance/Inputstream.h>

class AP4_ByteStream;

class ATTR_DLL_LOCAL TSReader : public TSDemux::TSDemuxer
{
public:
  TSReader(AP4_ByteStream *stream, uint32_t requiredMask);
  virtual ~TSReader();

  bool Initialize();

  virtual bool ReadAV(uint64_t pos, unsigned char * data, size_t len) override;

  void Reset(bool resetPackets = true);
  bool StartStreaming(AP4_UI32 typeMask);
  bool SeekTime(uint64_t timeInTs, bool preceeding);

  bool GetInformation(kodi::addon::InputstreamInfo& info);
  bool ReadPacket(bool streamInfo = false);
  /*!
   * \brief Read a frame left in the buffers when no data follows. The demuxer
   *        emits a PES only when the next PES starts, and a video parser emits
   *        a frame only when the next frame starts.
   */
  bool ReadEndPacket();

  /*!
   * \brief The data read from now on is a new segment. The session sets the
   *        offset of the first segment after Initialize() reads the stream
   *        information, so the first offset is the offset of that segment.
   * \param offset The PTS offset of the segment
   */
  void MarkSegmentStart(uint64_t offset);
  struct SegmentStart
  {
    uint64_t pts; // PTS of the first PES of the segment, of the video if the video is enabled
    uint64_t offset; // Offset of MarkSegmentStart()
  };
  /*!
   * \brief The start of the segment of the current packet. The demuxer emits
   *        a frame after later PES packets start, so the frame can be from a
   *        segment before the segment of the last read.
   * \return The start, or nothing while the PTS of the segment is unknown. An
   *         audio frame of a segment can come before the first video PES of
   *         the segment. Then the PTS diff of the segment before applies.
   */
  std::optional<SegmentStart> GetSegmentStart() const;
  /*!
   * \brief True if the packet is before the frame that the last seek found, in
   *        the stream of that frame. The read after the seek starts at the
   *        PES of the frame, and an audio PES holds more than one frame. The
   *        other streams start at the same position.
   */
  bool IsBeforeSeekFrame();

  uint64_t GetDts() const { return m_pkt.dts == PTS_UNSET ? PTS_UNSET : m_pkt.dts; }
  uint64_t GetPts() const { return m_pkt.pts == PTS_UNSET ? PTS_UNSET : m_pkt.pts; }
  uint64_t GetDuration() const { return m_pkt.duration; }
  const AP4_Byte *GetPacketData() const { return m_pkt.data; };
  const AP4_Size GetPacketSize() const { return static_cast<AP4_Size>(m_pkt.size); }
  const INPUTSTREAM_TYPE GetStreamType() const;

protected:
  /*!
   * \brief True if the data of the current segment ends at the position
   */
  virtual bool IsSegmentEnd(uint64_t position) { return false; }

private:
  bool GetPacket();
  bool HasVideo() const;
  void UpdateSegmentPts(TSDemux::ElementaryStream* stream);
  bool HandleProgramChange();
  bool HandleStreamChange(uint16_t pid);

  TSDemux::AVContext* m_AVContext{nullptr};

  AP4_ByteStream *m_stream;

  TSDemux::STREAM_PKT m_pkt;
  AP4_Position m_startPos;
  uint32_t m_requiredMask;
  uint32_t m_typeMask;
  bool m_stopAtSegmentEnd{false}; // ReadPacket fails at the end of the current segment
  struct Segment
  {
    uint32_t number;
    std::optional<uint64_t> offset;
    std::vector<std::pair<uint16_t, uint64_t>> pts; // PID and PTS of the first PES of each stream
  };
  // The last segments, the buffers hold frames of a few. The first segment is
  // the segment of the stream information.
  std::deque<Segment> m_segments{{0, {}, {}}};
  uint32_t m_segment{0};
  struct SeekFrame
  {
    uint16_t pid;
    uint64_t pts;
  };
  std::optional<SeekFrame> m_seekFrame;

  struct TSINFO
  {
    TSINFO(TSDemux::ElementaryStream* stream) : m_stream(stream), m_needInfo(true), m_changed(false), m_enabled(false), m_streamType(INPUTSTREAM_TYPE_NONE) {};

    TSDemux::ElementaryStream* m_stream;
    bool m_needInfo, m_changed, m_enabled;
    INPUTSTREAM_TYPE m_streamType;
  };
  std::vector<TSINFO> m_streamInfos;
};
