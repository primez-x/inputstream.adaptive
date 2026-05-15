/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ClearKeyCencSingleSampleDecrypter.h"

#include "ClearKeyDecrypter.h"
#include "CompSettings.h"
#include "SrvBroker.h"
#include "utils/Base64Utils.h"
#include "utils/CurlUtils.h"
#include "utils/FileUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <utility>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace UTILS;

namespace
{
void CkB64Encode(std::string& str)
{
  STRING::ReplaceAll(str, "+", "-");
  STRING::ReplaceAll(str, "/", "_");
}

void CkB64Decode(std::string& str)
{
  STRING::ReplaceAll(str, "-", "+");
  STRING::ReplaceAll(str, "_", "/");
}
}

CClearKeyCencSingleSampleDecrypter::CClearKeyCencSingleSampleDecrypter(
    std::string_view licenseUrl,
    const std::map<std::string, std::string>& licenseHeaders,
    const std::vector<uint8_t>& defaultKeyId,
    CClearKeyDecrypter* host)
  : m_host(host)
{
  if (licenseUrl.empty())
  {
    LOG::LogF(LOGERROR, "License server URL not found");
    return;
  }

  const std::string postData = CreateLicenseRequest(defaultKeyId);

  if (CSrvBroker::GetSettings().IsDebugLicense())
  {
    const std::string debugFilePath =
        FILESYS::PathCombine(m_host->GetLibraryPath(), "ClearKey.init");
    FILESYS::SaveFile(debugFilePath, postData.c_str(), true);
  }

  CURL::CUrl curl{licenseUrl, postData};
  curl.AddHeader("Accept", "application/json");
  curl.AddHeader("Content-Type", "application/json");
  curl.AddHeaders(licenseHeaders);

  std::string response;
  int statusCode = curl.Open();
  if (statusCode == -1 || statusCode >= 400)
  {
    LOG::Log(LOGERROR, "License server returned failure (HTTP error %i)", statusCode);
    return;
  }

  if (curl.Read(response) != CURL::ReadStatus::IS_EOF)
  {
    LOG::LogF(LOGERROR, "Could not read the license server response");
    return;
  }

  if (CSrvBroker::GetSettings().IsDebugLicense())
  {
    const std::string debugFilePath =
        FILESYS::PathCombine(m_host->GetLibraryPath(), "ClearKey.response");
    FILESYS::SaveFile(debugFilePath, response, true);
  }

  if (!ParseLicenseResponse(response))
  {
    LOG::LogF(LOGERROR, "Could not parse the license server response");
    return;
  }

  if (!STRING::KeyExists(m_kidPairs, defaultKeyId))
  {
    LOG::LogF(LOGERROR, "Key not found on license server response");
    m_kidPairs.clear();
    return;
  }

  SetParentIsOwner(false);
}

CClearKeyCencSingleSampleDecrypter::CClearKeyCencSingleSampleDecrypter(
    const std::vector<uint8_t>& initData,
    const std::vector<uint8_t>& defaultKeyId,
    const std::map<std::string, std::string>& keys,
    CClearKeyDecrypter* host)
  : m_host(host)
{
  std::vector<uint8_t> hexKey;

  if (keys.empty()) // Assume key is provided from the manifest / HLS EXT-X-KEY URI
  {
    hexKey = initData;
  }
  else // Key provided via Kodi drm_legacy / DRM config props
  {
    const std::string hexDefKid = STRING::ToHexadecimal(defaultKeyId);

    if (STRING::KeyExists(keys, hexDefKid))
      STRING::ToHexBytes(keys.at(hexDefKid), hexKey);
    else
      LOG::LogF(LOGERROR, "Missing KeyId \"%s\" on DRM configuration", hexDefKid.c_str());
  }

  if (!hexKey.empty())
    m_kidPairs.emplace(defaultKeyId, std::move(hexKey));

  SetParentIsOwner(false);
}

bool CClearKeyCencSingleSampleDecrypter::HasKeyId(const std::vector<uint8_t>& keyid)
{
  return !keyid.empty() && STRING::KeyExists(m_kidPairs, keyid);
}

AP4_UI32 CClearKeyCencSingleSampleDecrypter::AddPool()
{
  const AP4_UI32 poolId = m_nextPoolId++;
  m_pool.emplace(poolId, PINFO());
  return poolId;
}

void CClearKeyCencSingleSampleDecrypter::RemovePool(AP4_UI32 poolId)
{
  m_pool.erase(poolId);
}

AP4_Result CClearKeyCencSingleSampleDecrypter::SetFragmentInfo(AP4_UI32 poolId,
                                                               const std::vector<uint8_t>& keyId,
                                                               const AP4_UI08 nalLengthSize,
                                                               AP4_DataBuffer& annexbSpsPps,
                                                               AP4_UI32 flags,
                                                               CryptoInfo cryptoInfo)
{
  if (!STRING::KeyExists(m_pool, poolId))
  {
    LOG::LogF(LOGERROR, "Cannot set fragment info, pool id %u does not exist", poolId);
    return AP4_ERROR_INVALID_PARAMETERS;
  }

  if (cryptoInfo.m_mode != CryptoMode::NONE && cryptoInfo.m_mode != CryptoMode::AES_CTR &&
      cryptoInfo.m_mode != CryptoMode::AES_CBC)
  {
    LOG::LogF(LOGERROR, "Cannot set fragment info, unsupported crypto mode (%i)",
              static_cast<int>(cryptoInfo.m_mode));
    return AP4_ERROR_INVALID_PARAMETERS;
  }

  PINFO& pInfo = m_pool[poolId];
  FINFO& fInfo = pInfo.fInfo;

  // If any encryption parameter changed since the previous fragment, the
  // underlying AP4 decrypter will have to be re-created on the next sample.
  pInfo.isChanged = fInfo.kid != keyId || fInfo.cryptoInfo.m_mode != cryptoInfo.m_mode ||
                    fInfo.cryptoInfo.m_cryptBlocks != cryptoInfo.m_cryptBlocks ||
                    fInfo.cryptoInfo.m_skipBlocks != cryptoInfo.m_skipBlocks;

  fInfo.kid = keyId;
  fInfo.cryptoInfo = cryptoInfo;

  return AP4_SUCCESS;
}

AP4_Result CClearKeyCencSingleSampleDecrypter::DecryptSampleData(
    AP4_UI32 poolId,
    AP4_DataBuffer& dataIn,
    AP4_DataBuffer& dataOut,
    const AP4_UI08* iv,
    unsigned int subsampleCount,
    const AP4_UI16* bytesOfCleartextData,
    const AP4_UI32* bytesOfEncryptedData)
{
  if (m_pool.empty())
  {
    LOG::LogF(LOGERROR, "Cannot decrypt data, the pool is empty");
    return AP4_ERROR_INTERNAL;
  }
  if (!STRING::KeyExists(m_pool, poolId))
  {
    LOG::LogF(LOGERROR, "Cannot decrypt data, pool id %u does not exist", poolId);
    return AP4_ERROR_INVALID_PARAMETERS;
  }

  PINFO& pInfo = m_pool[poolId];
  auto& decrypter = pInfo.decrypter;

  if (!decrypter || pInfo.isChanged)
  {
    const auto& fInfo = pInfo.fInfo;

    // Empty kid: most HLS ClearKey streams identify the key only by URI (no KID
    // declared on EXT-X-KEY). In that case use the single key we have.
    std::vector<uint8_t> lookupKid = fInfo.kid;
    if (lookupKid.empty() && m_kidPairs.size() == 1)
      lookupKid = m_kidPairs.begin()->first;

    if (!STRING::KeyExists(m_kidPairs, lookupKid))
    {
      LOG::LogF(LOGERROR, "Cannot decrypt data, missing KID/KEY from the license data");
      return AP4_ERROR_INVALID_STATE;
    }

    const std::vector<uint8_t>& key = m_kidPairs[lookupKid];

    AP4_UI32 cypherType{AP4_CENC_CIPHER_NONE};
    bool resetIvEachSubsample{false};

    if (fInfo.cryptoInfo.m_mode == CryptoMode::AES_CBC)
    {
      cypherType = AP4_CENC_CIPHER_AES_128_CBC;
      // CBCS resets the IV at each subsample.
      // See https://github.com/axiomatic-systems/Bento4/commit/ab07e3acc7befc821554bc5e271df1201681e954
      resetIvEachSubsample = true;
    }
    else
    {
      // CryptoMode::AES_CTR (and NONE — DASH path that never sets a mode falls here
      // and matches the pre-CBCS Omega behavior).
      cypherType = AP4_CENC_CIPHER_AES_128_CTR;
    }

    AP4_CencSingleSampleDecrypter* pDecrypter{nullptr};
    if (AP4_FAILED(AP4_CencSingleSampleDecrypter::Create(
            cypherType, key.data(), static_cast<AP4_Size>(key.size()),
            fInfo.cryptoInfo.m_cryptBlocks, fInfo.cryptoInfo.m_skipBlocks, nullptr,
            resetIvEachSubsample, pDecrypter)))
    {
      LOG::LogF(LOGERROR, "Failed to create AP4_CencSingleSampleDecrypter");
      return AP4_ERROR_INTERNAL;
    }

    decrypter.reset(pDecrypter);
    pInfo.isChanged = false;
  }

  return decrypter->DecryptSampleData(dataIn, dataOut, iv, subsampleCount, bytesOfCleartextData,
                                      bytesOfEncryptedData);
}

std::string CClearKeyCencSingleSampleDecrypter::CreateLicenseRequest(
    const std::vector<uint8_t>& defaultKeyId)
{
  // github.com/Dash-Industry-Forum/ClearKey-Content-Protection/blob/master/README.md
  /* Expected JSON structure for license request:
   * { "kids":
   *     [
   *         "nrQFDeRLSAKTLifXUIPiZg"
   *     ]
   * "type":"temporary" }
   */

  std::string b64Kid = BASE64::Encode(defaultKeyId, false);
  CkB64Encode(b64Kid);

  rapidjson::Document jDoc;
  jDoc.SetObject();
  auto& allocator = jDoc.GetAllocator();

  rapidjson::Value kids{rapidjson::kArrayType};
  rapidjson::Value jKid;

  jKid.SetString(b64Kid.c_str(), allocator);
  kids.PushBack(jKid, allocator);

  jDoc.AddMember("kids", kids, allocator);
  jDoc.AddMember("type", "temporary", allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer{buffer};
  jDoc.Accept(writer);
  return buffer.GetString();
}

bool CClearKeyCencSingleSampleDecrypter::ParseLicenseResponse(std::string data)
{
  /* Expected JSON structure for license response:
   * { "keys": [
   *     {
   *         "k": "FmY0xnWCPCNaSpRG-tUuTQ",
   *         "kid": "nrQFDeRLSAKTLifXUIPiZg",
   *         "kty": "oct"
   *     }
   * "type": "temporary"}
   */

  rapidjson::Document jDoc;
  jDoc.Parse(data.c_str(), data.size());

  if (!jDoc.IsObject())
  {
    LOG::LogF(LOGERROR, "Malformed JSON data in license response");
    return false;
  }

  if (jDoc.HasMember("Message") && jDoc["Message"].IsString())
  {
    LOG::LogF(LOGERROR, "Error in license response: %s", jDoc["Message"].GetString());
    return false;
  }

  if (!jDoc.HasMember("keys") || !jDoc["keys"].IsArray())
  {
    LOG::LogF(LOGERROR, "No keys in license response");
    return false;
  }

  for (auto const& jArrayKey : jDoc["keys"].GetArray())
  {
    if (!jArrayKey.IsObject())
      continue;

    if (jArrayKey.HasMember("kty") && jArrayKey["kty"].IsString() &&
        std::string{jArrayKey["kty"].GetString()} != "oct")
    {
      LOG::LogF(LOGWARNING, "Ignored JWK set, due to unsupported \"%s\" key type",
                jArrayKey["kty"].GetString());
      continue;
    }

    std::string b64Key;
    std::string b64KeyId;

    if (jArrayKey.HasMember("k") && jArrayKey["k"].IsString())
      b64Key = jArrayKey["k"].GetString();

    if (jArrayKey.HasMember("kid") && jArrayKey["kid"].IsString())
      b64KeyId = jArrayKey["kid"].GetString();

    if (b64Key.empty() || b64KeyId.empty())
      continue;

    CkB64Decode(b64Key);
    BASE64::AddPadding(b64Key);
    CkB64Decode(b64KeyId);
    BASE64::AddPadding(b64KeyId);

    const std::vector<uint8_t> kidBytes = BASE64::Decode(b64KeyId);
    const std::vector<uint8_t> keyBytes = BASE64::Decode(b64Key);

    if (kidBytes.empty() || keyBytes.empty())
    {
      LOG::LogF(LOGERROR, "Malformed key pair value in license response");
      continue;
    }

    m_kidPairs.insert_or_assign(kidBytes, keyBytes);
  }

  return !m_kidPairs.empty();
}
