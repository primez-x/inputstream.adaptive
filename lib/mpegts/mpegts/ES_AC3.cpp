/*
 *  Copyright (C) 2005-2013 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ES_AC3.h"
#include "bitstream.h"

#include <algorithm>      // for max

using namespace TSDemux;

#define AC3_HEADER_SIZE 7
#define EAC3_DEPENDENT_HEADER_SIZE 6 // Up to bsid

/** Channel mode (audio coding mode) */
typedef enum
{
  AC3_CHMODE_DUALMONO = 0,
  AC3_CHMODE_MONO,
  AC3_CHMODE_STEREO,
  AC3_CHMODE_3F,
  AC3_CHMODE_2F1R,
  AC3_CHMODE_3F1R,
  AC3_CHMODE_2F2R,
  AC3_CHMODE_3F2R
} AC3ChannelMode;

/* possible frequencies */
const uint16_t AC3SampleRateTable[3] = { 48000, 44100, 32000 };

/* possible bitrates */
const uint16_t AC3BitrateTable[19] = {
    32, 40, 48, 56, 64, 80, 96, 112, 128,
    160, 192, 224, 256, 320, 384, 448, 512, 576, 640
};

const uint8_t AC3ChannelsTable[8] = {
    2, 1, 2, 3, 3, 4, 4, 5
};

const uint16_t AC3FrameSizeTable[38][3] = {
    { 64,   69,   96   },
    { 64,   70,   96   },
    { 80,   87,   120  },
    { 80,   88,   120  },
    { 96,   104,  144  },
    { 96,   105,  144  },
    { 112,  121,  168  },
    { 112,  122,  168  },
    { 128,  139,  192  },
    { 128,  140,  192  },
    { 160,  174,  240  },
    { 160,  175,  240  },
    { 192,  208,  288  },
    { 192,  209,  288  },
    { 224,  243,  336  },
    { 224,  244,  336  },
    { 256,  278,  384  },
    { 256,  279,  384  },
    { 320,  348,  480  },
    { 320,  349,  480  },
    { 384,  417,  576  },
    { 384,  418,  576  },
    { 448,  487,  672  },
    { 448,  488,  672  },
    { 512,  557,  768  },
    { 512,  558,  768  },
    { 640,  696,  960  },
    { 640,  697,  960  },
    { 768,  835,  1152 },
    { 768,  836,  1152 },
    { 896,  975,  1344 },
    { 896,  976,  1344 },
    { 1024, 1114, 1536 },
    { 1024, 1115, 1536 },
    { 1152, 1253, 1728 },
    { 1152, 1254, 1728 },
    { 1280, 1393, 1920 },
    { 1280, 1394, 1920 },
};

const uint8_t EAC3Blocks[4] = {
  1, 2, 3, 6
};

typedef enum {
  EAC3_FRAME_TYPE_INDEPENDENT = 0,
  EAC3_FRAME_TYPE_DEPENDENT,
  EAC3_FRAME_TYPE_AC3_CONVERT,
  EAC3_FRAME_TYPE_RESERVED
} EAC3FrameType;

ES_AC3::ES_AC3(uint16_t pid)
 : ElementaryStream(pid)
{
  m_PTS                       = 0;
  m_DTS                       = 0;
  m_FrameSize                 = 0;
  m_FrameDuration             = 0;
  m_Dependent                 = false;
  m_SampleRate                = 0;
  m_Channels                  = 0;
  m_BitRate                   = 0;
  es_alloc_init               = 1920*2;
}

ES_AC3::~ES_AC3()
{
}

void ES_AC3::Parse(STREAM_PKT* pkt)
{
  int p = es_parsed;
  int l;
  while (true)
  {
    while ((l = es_len - p) > 8)
    {
      if (FindHeaders(es_buf + p, l) < 0)
        break;
      p++;
    }
    es_parsed = p;
    // A dependent frame without the frame before, as at the start, is no
    // frame. The search continues after that frame.
    if (!es_found_frame || l < m_FrameSize || !m_Dependent)
      break;
    p += m_FrameSize;
    es_consumed = es_parsed = p;
    es_found_frame = false;
  }

  if (es_found_frame && l >= m_FrameSize)
  {
    // The dependent frames after a frame extend the channels of that frame and
    // belong to the frame, as in FFmpeg
    int size = m_FrameSize;
    while (true)
    {
      const int rest = l - size;
      const int dependent = DependentFrameSize(es_buf + p + size, rest);
      if (dependent > 0 && rest >= dependent)
        size += dependent;
      // No dependent frame follows. At the end of the data, nothing can follow
      // too: the end of a segment between the frames looks the same as the
      // end of the stream.
      else if (!dependent || (es_end_of_data && !rest))
        break;
      else
        return; // The rest of the dependent frame or of the header follows
    }
    bool streamChange = SetAudioInformation(m_Channels, m_SampleRate, m_BitRate, 0, 0);
    pkt->pid            = pid;
    pkt->data           = &es_buf[p];
    pkt->size           = size;
    pkt->duration       = m_FrameDuration;
    pkt->dts            = m_DTS;
    pkt->pts            = m_PTS;
    pkt->streamChange   = streamChange;

    es_consumed = p + size;
    es_parsed = es_consumed;
    es_found_frame = false;
  }
}

int ES_AC3::DependentFrameSize(const uint8_t* buf, int size)
{
  // The sync word and the frame type of a dependent frame
  if ((size >= 1 && buf[0] != 0x0b) || (size >= 2 && buf[1] != 0x77) ||
      (size >= 3 && buf[2] >> 6 != EAC3_FRAME_TYPE_DEPENDENT))
    return 0;
  if (size < EAC3_DEPENDENT_HEADER_SIZE)
    return -1;
  // The header checks of FindHeaders(): bsid 11 to 16 of E-AC-3, the frame
  // size and the sample rate codes
  CBitstream bs(const_cast<uint8_t*>(buf) + 2, (EAC3_DEPENDENT_HEADER_SIZE - 2) * 8);
  const int bsid = bs.showBits(29) & 0x1F;
  if (bsid <= 10 || bsid > 16)
    return 0;
  bs.skipBits(2 + 3); // frame type, substreamid
  const int frameSize = (bs.readBits(11) + 1) << 1;
  if (frameSize < AC3_HEADER_SIZE || bs.readBits(4) == 0xF)
    return 0;
  return frameSize;
}

int ES_AC3::FindHeaders(uint8_t *buf, int buf_size)
{
  if (es_found_frame)
    return -1;

  if (buf_size < 9)
    return -1;

  uint8_t *buf_ptr = buf;

  if ((buf_ptr[0] == 0x0b && buf_ptr[1] == 0x77))
  {
    CBitstream bs(buf_ptr + 2, AC3_HEADER_SIZE * 8);

    // read ahead to bsid to distinguish between AC-3 and E-AC-3
    int bsid = bs.showBits(29) & 0x1F;
    if (bsid > 16)
      return 0;

    if (bsid <= 10)
    {
      // Normal AC-3
      bs.skipBits(16);
      int fscod       = bs.readBits(2);
      int frmsizecod  = bs.readBits(6);
      bs.skipBits(5); // skip bsid, already got it
      bs.skipBits(3); // skip bitstream mode
      int acmod       = bs.readBits(3);

      if (fscod == 3 || frmsizecod > 37)
        return 0;

      if (acmod == AC3_CHMODE_STEREO)
      {
        bs.skipBits(2); // skip dsurmod
      }
      else
      {
        if ((acmod & 1) && acmod != AC3_CHMODE_MONO)
          bs.skipBits(2);
        if (acmod & 4)
          bs.skipBits(2);
      }
      int lfeon = bs.readBits(1);

      int srShift   = std::max(bsid, 8) - 8;
      m_SampleRate  = AC3SampleRateTable[fscod] >> srShift;
      m_BitRate     = (AC3BitrateTable[frmsizecod>>1] * 1000) >> srShift;
      m_Channels    = AC3ChannelsTable[acmod] + lfeon;
      m_FrameSize   = AC3FrameSizeTable[frmsizecod][fscod] * 2;
      m_FrameDuration = 90000 * 1536 / m_SampleRate;
      m_Dependent = false;
    }
    else
    {
      // Enhanced AC-3
      int frametype = bs.readBits(2);
      if (frametype == EAC3_FRAME_TYPE_RESERVED)
        return 0;

       bs.readBits(3); // int substreamid

      m_FrameSize = (bs.readBits(11) + 1) << 1;
      if (m_FrameSize < AC3_HEADER_SIZE)
        return 0;

      int numBlocks = 6;
      int sr_code = bs.readBits(2);
      if (sr_code == 3)
      {
        int sr_code2 = bs.readBits(2);
        if (sr_code2 == 3)
          return 0;
        m_SampleRate = AC3SampleRateTable[sr_code2] / 2;
      }
      else
      {
        numBlocks = EAC3Blocks[bs.readBits(2)];
        m_SampleRate = AC3SampleRateTable[sr_code];
      }

      int channelMode = bs.readBits(3);
      int lfeon = bs.readBits(1);

      m_BitRate  = (uint32_t)(8.0 * m_FrameSize * m_SampleRate / (numBlocks * 256.0));
      m_Channels = AC3ChannelsTable[channelMode] + lfeon;
      m_FrameDuration = 90000 * numBlocks * 256 / m_SampleRate;
      m_Dependent = frametype == EAC3_FRAME_TYPE_DEPENDENT;
    }
    es_found_frame = true;
    if (!m_Dependent)
      m_DTS = m_PTS = AudioFramePts(buf - es_buf, m_FrameDuration);
    return -1;
  }
  return 0;
}

void ES_AC3::Reset()
{
  ElementaryStream::Reset();
}
