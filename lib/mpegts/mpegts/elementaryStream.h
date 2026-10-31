/*
 *  Copyright (C) 2013 Jean-Luc Barriere
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#ifndef ELEMENTARYSTREAM_H
#define ELEMENTARYSTREAM_H

#include <inttypes.h>
#include <cstddef>    // for size_t
#include <utility>
#include <vector>

#define ES_INIT_BUFFER_SIZE     64000
#define ES_MAX_BUFFER_SIZE      1048576
#define PTS_MASK                0x1ffffffffLL
#define PTS_UNSET               0x1ffffffffLL
#define PTS_TIME_BASE           90000LL
#define RESCALE_TIME_BASE       1000000LL

namespace TSDemux
{
  enum STREAM_TYPE
  {
    STREAM_TYPE_UNKNOWN = 0,
    STREAM_TYPE_VIDEO_MPEG1,
    STREAM_TYPE_VIDEO_MPEG2,
    STREAM_TYPE_AUDIO_MPEG1,
    STREAM_TYPE_AUDIO_MPEG2,
    STREAM_TYPE_AUDIO_AAC,
    STREAM_TYPE_AUDIO_AAC_ADTS,
    STREAM_TYPE_AUDIO_AAC_LATM,
    STREAM_TYPE_VIDEO_H264,
    STREAM_TYPE_VIDEO_HEVC,
    STREAM_TYPE_AUDIO_AC3,
    STREAM_TYPE_AUDIO_EAC3,
    STREAM_TYPE_DVB_TELETEXT,
    STREAM_TYPE_DVB_SUBTITLE,
    STREAM_TYPE_VIDEO_MPEG4,
    STREAM_TYPE_VIDEO_VC1,
    STREAM_TYPE_AUDIO_LPCM,
    STREAM_TYPE_AUDIO_DTS,
    STREAM_TYPE_PRIVATE_DATA
  };

  struct STREAM_INFO
  {
    char                  language[4];
    int                   composition_id;
    int                   ancillary_id;
    int codecProfile;
    int                   fps_scale;
    int                   fps_rate;
    int                   height;
    int                   width;
    float                 aspect;
    int                   channels;
    int                   sample_rate;
    int                   block_align;
    int                   bit_rate;
    int                   bits_per_sample;
    bool                  interlaced;
    uint8_t               extra_data[1024];
    int                   extra_data_size;
  };

  struct STREAM_PKT
  {
    uint16_t              pid;
    size_t                size;
    const unsigned char*  data;
    int64_t               dts;
    int64_t               pts;
    uint64_t              duration;
    bool                  streamChange;
    bool                  recoveryPoint;
  };

  class ElementaryStream
  {
  public:
    ElementaryStream(uint16_t pes_pid);
    virtual ~ElementaryStream();
    virtual void Reset();
    void ClearBuffer();
    int Append(const unsigned char* buf, size_t len, bool new_pts = false);
    const char* GetStreamCodecName() const;
    static const char* GetStreamCodecName(STREAM_TYPE stream_type);

    uint16_t pid;
    STREAM_TYPE stream_type;
    int64_t c_dts;               ///< current MPEG stream DTS (decode time for video)
    int64_t c_pts;               ///< current MPEG stream PTS (presentation time for audio and video)
    int64_t p_dts;               ///< previous MPEG stream DTS (decode time for video)
    int64_t p_pts;               ///< previous MPEG stream PTS (presentation time for audio and video)
    uint32_t pes_pts_count;      ///< number of PES headers with a PTS

    bool has_stream_info;         ///< true if stream info is completed else it requires parsing of iframe

    STREAM_INFO stream_info;

    bool GetStreamPacket(STREAM_PKT* pkt);
    bool GetEndPacket(STREAM_PKT* pkt); ///< Like GetStreamPacket(), but the buffer ends with the end of the last frame
    void ContinueDecode(bool continueDecode) { es_continue_decode = continueDecode; } ///< After the next reset, the frames continue the decode of a key frame that the reader returned
    virtual void Parse(STREAM_PKT* pkt);

    struct PesStart
    {
      size_t offset;              ///< Position in buffer where the PES data starts
      uint64_t position;          ///< Stream position of the PES
      uint32_t segment;           ///< Media segment of the PES
      bool timestamped;           ///< The PES has a PTS
      int64_t pts;                ///< The PTS of the PES data: of the PES, or of the last PES with a PTS before
      int64_t dts;                ///< The DTS of the PES data
    };
    void StartPes(uint64_t position, uint32_t segment); ///< The next appended data starts a PES
    const PesStart* GetPesStart(const unsigned char* data) const; ///< The PES where packet data starts
    uint32_t GetPesSegment() const { return es_pes_start_segment; } ///< Media segment of the last PES start

  protected:
    void ResetStreamPacket(STREAM_PKT* pkt);
    uint64_t Rescale(uint64_t a, uint64_t b, uint64_t c);
    bool SetVideoInformation(int FpsScale, int FpsRate, int Height, int Width, float Aspect, bool Interlaced);
    bool SetAudioInformation(int Channels, int SampleRate, int BitRate, int BitsPerSample, int BlockAlign, int codecProfile = 0);

    size_t es_alloc_init;         ///< Initial allocation of memory for buffer
    unsigned char* es_buf;        ///< The Pointer to buffer
    size_t es_alloc;              ///< Allocated size of memory for buffer
    size_t es_len;                ///< Size of data in buffer
    size_t es_consumed;           ///< Consumed payload. Will be erased on next append
    size_t es_parsed;             ///< Parser: Last processed position in buffer
    bool   es_end_of_data;        ///< Parser: In GetEndPacket(), the last frame ends at the end of the buffer
    bool   es_skip_to_pts;        ///< Parser: The end of the data completed the last frame. The rest of that frame follows up to the next PES with a PTS.
    bool   es_continue_decode;    ///< Parser: After the next reset, the frames continue a decode
    bool TakeContinueDecode() { return std::exchange(es_continue_decode, false); } ///< Parser: In Reset(), the frames continue a decode
    std::vector<PesStart> es_pes_starts; ///< The PES starts in buffer order, the first PES can start before the buffer
    bool   es_pes_start_pending;
    uint64_t es_pes_start_position;
    uint32_t es_pes_start_segment;
    void ShiftPesStarts(size_t consumed);
    const PesStart* StartPicture(size_t offset); ///< Parser: The first picture of the found frame starts at offset. Returns the PES of the frame.
    uint64_t FrameDuration(int64_t dts, uint64_t fallback); ///< Parser: The duration of the frame with the DTS that ends at es_consumed
    int64_t AudioFramePts(size_t offset, int64_t duration); ///< Parser: The PTS of the audio frame that starts at offset
    void EndLastFrame(); ///< Parser: In GetEndPacket(), the end of the found frame
    bool AppendExtraData(const uint8_t* nal, size_t size); ///< Appends a NAL unit with a 4 byte start code
    bool   es_found_frame;        ///< Parser: Found frame
    size_t es_picture_start;      ///< Parser: The first picture of the found frame, relative to es_consumed
    uint64_t es_frame_duration;   ///< Parser: The last duration from the DTS of two frames
    uint64_t es_pts_position;     ///< Parser: The stream position of the PES of the last audio frame PTS
    int64_t es_next_pts;          ///< Parser: The PTS of the next audio frame
    bool   es_frame_valid;
    bool   es_extraDataChanged;
  };
}

#endif /* ELEMENTARYSTREAM_H */
