/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>

// The demuxer tests build without common/AdaptiveStream.cpp. This stream copies
// the read, seek and tell rules of AdaptiveStream for downloaded segments: a
// read never spans two segments, a seek stays in the current segment, a failed
// seek moves only the segment read position, and tell can switch segments.
// A fixated initialization segment does not switch.
// A switch calls the observer with the segment start time, as
// CSession::OnSegmentChanged sets the PTS offset of the sample reader.
namespace adaptive
{
class AdaptiveStream
{
public:
  bool ensureSegment()
  {
    if (!m_fixateInitialization && m_segments.size() > 1 &&
        m_segmentReadPos >= m_segments.front().size())
    {
      m_segments.pop_front();
      m_starts.pop_front();
      m_segmentReadPos = 0;
      m_onSegmentChanged(m_starts.front());
    }
    return !m_segments.empty() && m_segmentReadPos < m_segments.front().size();
  }

  uint32_t read(void* buffer, uint32_t bytesToRead)
  {
    while (ensureSegment() && bytesToRead > 0)
    {
      const size_t avail{std::min<size_t>(bytesToRead,
                                          m_segments.front().size() - m_segmentReadPos)};
      m_segmentReadPos += avail;
      m_absolutePosition += avail;
      if (avail < bytesToRead)
        break; // AdaptiveStream drops the bytes of a short read
      std::memcpy(buffer, m_segments.front().data() + m_segmentReadPos - avail, avail);
      return bytesToRead;
    }
    return 0;
  }

  bool seek(uint64_t pos)
  {
    if (m_segments.empty() || pos < m_absolutePosition - m_segmentReadPos)
      return false;
    m_segmentReadPos = static_cast<size_t>(pos - (m_absolutePosition - m_segmentReadPos));
    if (m_segmentReadPos > m_segments.front().size())
    {
      m_segmentReadPos = m_segments.front().size();
      return false;
    }
    m_absolutePosition = pos;
    return true;
  }

  std::deque<std::vector<uint8_t>> m_segments; // The current segment first
  std::deque<uint64_t> m_starts; // The start time of each segment
  std::function<void(uint64_t)> m_onSegmentChanged;
  size_t m_segmentReadPos{0};
  uint64_t m_absolutePosition{0};
  bool m_waitingForSegment{false};
  bool m_fixateInitialization{false}; // Reads stay in the initialization segment
};
} // namespace adaptive
