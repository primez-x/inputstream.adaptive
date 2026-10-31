/*
 *  Copyright (C) 2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SampleReader.h"
#include "demuxers/TSReader.h"

#include <deque>
#include <optional>
#include <vector>

// forwards
class CAdaptiveByteStream;

class ATTR_DLL_LOCAL CTSSampleReader : public ISampleReader, public TSReader
{
public:
  CTSSampleReader(AP4_ByteStream* input,
                  INPUTSTREAM_TYPE type,
                  AP4_UI32 streamId,
                  uint32_t requiredMask);

  bool Initialize(SESSION::CStream* stream) override;
  void AddStreamType(INPUTSTREAM_TYPE type, uint32_t sid) override;
  void SetStreamType(INPUTSTREAM_TYPE type, uint32_t sid) override;
  bool RemoveStreamType(INPUTSTREAM_TYPE type) override;
  bool IsStarted() const override { return m_started; }
  bool EOS() const override { return m_eos; }
  uint64_t DTS() const override { return m_dts; }
  uint64_t PTS() const override { return m_pts; }
  AP4_Result Start(bool& bStarted) override;
  AP4_Result ReadSample() override;
  void Reset(bool bEOS) override;
  bool GetInformation(kodi::addon::InputstreamInfo& info) override
  {
    return TSReader::GetInformation(info);
  }
  bool TimeSeek(uint64_t pts, bool preceeding) override;
  void SetPTSOffset(uint64_t offset) override;
  int64_t GetPTSDiff() const override { return m_ptsDiff; }
  uint32_t GetTimeScale() const override { return 90000; }
  AP4_UI32 GetStreamId() const override
  {
    return m_sample ? m_sample->streamId : m_typeMap[GetStreamType()];
  }
  AP4_Size GetSampleDataSize() const override
  {
    return m_sample ? static_cast<AP4_Size>(m_sample->data.size()) : GetPacketSize();
  }
  const AP4_Byte* GetSampleData() const override
  {
    return m_sample ? m_sample->data.data() : GetPacketData();
  }
  uint64_t GetDuration() const override
  {
    return ((m_sample ? m_sample->duration : TSReader::GetDuration()) * 100) / 9;
  }
  bool IsEncrypted() const override { return false; }

protected:
  bool IsSegmentEnd(uint64_t position) override;

private:
  bool Seek(uint64_t pts);
  AP4_Result EndOfData();

  uint32_t m_typeMask; //Bit representation of INPUTSTREAM_TYPES
  uint32_t m_typeMap[16];
  uint64_t m_pts{0};
  uint64_t m_dts{0};
  int64_t m_ptsDiff{0};
  bool m_eos{false};
  bool m_started{false};
  std::optional<uint64_t> m_seekTime; // A seek that waits for a live segment
  bool m_seekPreceeding{false};
  struct Sample
  {
    AP4_UI32 streamId;
    uint64_t dts;
    uint64_t pts;
    uint64_t duration;
    int64_t ptsDiff;
    std::vector<AP4_Byte> data;
  };
  // The session aligns the other streams to the PTS of the frame of a seek,
  // so that frame comes first. The samples of other streams read before the
  // frame follow the frame.
  std::deque<Sample> m_samples;
  std::optional<Sample> m_sample; // The current sample, from m_samples
  CAdaptiveByteStream* m_adByteStream;
};
