/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AdaptiveByteStream.h"
#include "TestAdaptiveStream.h"

// The add-on entry point defines the Kodi interface, the demuxer tests do not use it
AddonGlobalInterface* kodi::addon::CPrivateBase::m_interface{nullptr};

AP4_Result CAdaptiveByteStream::ReadPartial(void* buffer, AP4_Size bytesToRead, AP4_Size& bytesRead)
{
  bytesRead = m_adStream->read(buffer, bytesToRead);
  return bytesRead > 0 ? AP4_SUCCESS : AP4_ERROR_READ_FAILED;
}

AP4_Result CAdaptiveByteStream::WritePartial(const void* buffer,
                                            AP4_Size bytesToWrite,
                                            AP4_Size& bytesWritten)
{
  return AP4_ERROR_NOT_SUPPORTED;
}

AP4_Result CAdaptiveByteStream::Seek(AP4_Position position)
{
  return m_adStream->seek(position) ? AP4_SUCCESS : AP4_ERROR_NOT_SUPPORTED;
}

AP4_Result CAdaptiveByteStream::Tell(AP4_Position& position)
{
  // AdaptiveStream::tell calls read(0, 0), which can switch to the next segment
  m_adStream->ensureSegment();
  position = m_adStream->m_absolutePosition;
  return AP4_SUCCESS;
}

AP4_Result CAdaptiveByteStream::GetSize(AP4_LargeSize& size)
{
  return AP4_ERROR_NOT_SUPPORTED;
}

bool CAdaptiveByteStream::waitingForSegment() const
{
  return m_adStream->m_waitingForSegment;
}

bool CAdaptiveByteStream::IsSegmentEnd(AP4_Position position)
{
  return m_adStream->IsSegmentEnd(position);
}

void CAdaptiveByteStream::FixateInitialization(bool on)
{
  m_adStream->m_fixateInitialization = on;
}

void CAdaptiveByteStream::SetSegmentFileOffset(uint64_t offset)
{
}
