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
#include <map>
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
   * \param oldestUsed The oldest segment that a frame still needs, as a held
   *        audio frame for the PTS diff of the frame
   */
  void MarkSegmentStart(uint64_t offset, std::optional<uint32_t> oldestUsed);
  struct SegmentStart
  {
    uint64_t pts; // PTS of the first frame of the segment, of the video if the video is enabled
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
  std::optional<SegmentStart> GetSegmentStart(uint32_t segment) const;
  /*!
   * \brief The segment of the PES where the current packet starts. Nothing for
   *        a copy of a frame.
   */
  std::optional<uint32_t> GetPacketSegment() const;
  /*!
   * \brief An audio frame with a PTS before the PTS of the stream in the
   *        segment sets that PTS. A PES without a PTS can start the segment.
   */
  void UpdateSegmentFramePts(uint32_t segment, uint16_t pid, uint64_t pts);
  /*!
   * \brief True if the packet is before the frame that the last seek found, in
   *        the stream of that frame. The read after the seek starts at the
   *        PES of the frame, and an audio PES holds more than one frame. The
   *        other streams start at the same position.
   */
  bool IsBeforeSeekFrame();
  /*!
   * \brief The PTS of the frame that the last seek found, until the frame is read
   */
  std::optional<uint64_t> GetSeekFramePts() const
  {
    return m_seekFrame ? std::optional<uint64_t>{m_seekFrame->pts} : std::nullopt;
  }

  uint64_t GetDts() const { return m_pkt.dts == PTS_UNSET ? PTS_UNSET : m_pkt.dts; }
  uint64_t GetPts() const { return m_pkt.pts == PTS_UNSET ? PTS_UNSET : m_pkt.pts; }
  uint64_t GetDuration() const { return m_pkt.duration; }
  uint16_t GetPid() const { return m_pkt.pid; }
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
  bool ReadNextPacket(bool scanStreamInfo);
  bool ReadNextEndPacket();
  /*!
   * \brief The packet of a read, if the read returned one, sets the state that
   *        each packet sets
   */
  bool PacketRead(bool read);
  // A frame and the PES where the frame starts
  struct ClockFrame
  {
    TSDemux::STREAM_PKT pkt;
    TSDemux::ElementaryStream::PesStart pes;
  };
  /*!
   * \brief Reads on until the clock of each stream at its resume position is
   *        known, or up to the end of the segment
   * \param frames The frames read before, more frames are added
   * \param resume The position where each stream continues
   */
  std::map<uint16_t, TSDemux::ElementaryStream::Clock> ReadClocks(
      std::vector<ClockFrame>& frames, const std::map<uint16_t, uint64_t>& resume);
  void ResumeClocks(const std::map<uint16_t, TSDemux::ElementaryStream::Clock>& clocks);
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
  bool m_parsersReset{false}; // No packet was read since a reset of the parsers
  struct Segment
  {
    uint32_t number;
    std::optional<uint64_t> offset;
    std::vector<std::pair<uint16_t, uint64_t>> pts; // PID and PTS of the first frame of each stream
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
  // A frame of a PES that starts before the position where the read after a
  // seek starts. The stream cannot read the segment before again.
  struct FrameCopy
  {
    TSDemux::STREAM_PKT pkt;
    std::vector<uint8_t> data;
    std::optional<SegmentStart> segmentStart;
    uint64_t pesPosition;
    uint64_t nextPosition; // The PES of the next frame, see ElementaryStream::NextFramePosition()
  };
  std::deque<FrameCopy> m_frameCopies; // The read after a seek starts with these frames
  std::optional<FrameCopy> m_frameCopy; // The data of the packet, if the packet is a copy
  // The frames of the last PES of the other streams, while a seek waits for
  // the next live segment. That segment can continue the PES.
  std::deque<FrameCopy> m_seekPesFrames;

  struct TSINFO
  {
    TSINFO(TSDemux::ElementaryStream* stream) : m_stream(stream), m_needInfo(true), m_changed(false), m_enabled(false), m_streamType(INPUTSTREAM_TYPE_NONE) {};

    TSDemux::ElementaryStream* m_stream;
    bool m_needInfo, m_changed, m_enabled;
    INPUTSTREAM_TYPE m_streamType;
  };
  std::vector<TSINFO> m_streamInfos;
};
