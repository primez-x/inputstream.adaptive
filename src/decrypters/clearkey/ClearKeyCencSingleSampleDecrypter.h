/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "common/AdaptiveCencSampleDecrypter.h"
#include "decrypters/IDecrypter.h"
#include "utils/CryptoUtils.h"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

class CClearKeyDecrypter;

class CClearKeyCencSingleSampleDecrypter : public Adaptive_CencSingleSampleDecrypter
{
public:
  CClearKeyCencSingleSampleDecrypter(std::string_view licenseUrl,
                                     const std::map<std::string, std::string>& licenseHeaders,
                                     const std::vector<uint8_t>& defaultKeyId,
                                     CClearKeyDecrypter* host);
  CClearKeyCencSingleSampleDecrypter(const std::vector<uint8_t>& initdata,
                                     const std::vector<uint8_t>& defaultKeyId,
                                     const std::map<std::string, std::string>& keys,
                                     CClearKeyDecrypter* host);
  virtual ~CClearKeyCencSingleSampleDecrypter(){};

  bool HasKeyId(const std::vector<uint8_t>& keyid);
  virtual AP4_UI32 AddPool() override;
  virtual void RemovePool(AP4_UI32 poolId) override;
  virtual AP4_Result SetFragmentInfo(AP4_UI32 poolId,
                                     const std::vector<uint8_t>& keyId,
                                     const AP4_UI08 nalLengthSize,
                                     AP4_DataBuffer& annexbSpsPps,
                                     AP4_UI32 flags,
                                     CryptoInfo cryptoInfo) override;
  virtual AP4_Result DecryptSampleData(AP4_UI32 poolId,
                                       AP4_DataBuffer& dataIn,
                                       AP4_DataBuffer& dataOut,
                                       const AP4_UI08* iv,
                                       unsigned int subsampleCount,
                                       const AP4_UI16* bytesOfCleartextData,
                                       const AP4_UI32* bytesOfEncryptedData) override;
  void SetDefaultKeyId(const std::vector<uint8_t>& keyId) override{};
  void AddKeyId(const std::vector<uint8_t>& keyId) override{};
  bool HasKeys() { return !m_kidPairs.empty(); }

private:
  std::string CreateLicenseRequest(const std::vector<uint8_t>& defaultKeyId);
  bool ParseLicenseResponse(std::string data);

  CClearKeyDecrypter* m_host;

  // KID (raw bytes) -> KEY (raw bytes)
  std::map<std::vector<uint8_t>, std::vector<uint8_t>> m_kidPairs;

  // Per-fragment cached state
  struct FINFO
  {
    std::vector<uint8_t> kid;
    CryptoInfo cryptoInfo;
  };

  // Per-pool decrypter + last-seen fragment info
  struct PINFO
  {
    FINFO fInfo;
    bool isChanged{false};
    std::unique_ptr<AP4_CencSingleSampleDecrypter> decrypter;
  };

  std::map<AP4_UI32, PINFO> m_pool;
  AP4_UI32 m_nextPoolId{0};
};
