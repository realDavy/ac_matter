#include "unique_commissionable_data_provider.h"

#include <esp_log.h>
#include <esp_random.h>

#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Base64.h>
#include <platform/ESP32/ESP32Config.h>
#include <setup_payload/SetupPayload.h>

#include <cinttypes>
#include <cstring>

static const char *TAG = "unique_cdp";

using chip::DeviceLayer::Internal::ESP32Config;

CHIP_ERROR unique_commissionable_data_provider::LoadFromNvs()
{
    uint32_t passcode = 0;
    uint32_t disc = 0;
    uint32_t iterations = 0;
    char salt_b64[BASE64_ENCODED_LEN(chip::Crypto::kSpake2p_Max_PBKDF_Salt_Length) + 1] = {};
    size_t salt_b64_len = 0;

    ReturnErrorOnFailure(ESP32Config::ReadConfigValue(ESP32Config::kConfigKey_SetupPinCode, passcode));
    ReturnErrorOnFailure(ESP32Config::ReadConfigValue(ESP32Config::kConfigKey_SetupDiscriminator, disc));
    ReturnErrorOnFailure(
        ESP32Config::ReadConfigValue(ESP32Config::kConfigKey_Spake2pIterationCount, iterations));
    ReturnErrorOnFailure(ESP32Config::ReadConfigValueStr(ESP32Config::kConfigKey_Spake2pSalt, salt_b64,
                                                         sizeof(salt_b64), salt_b64_len));

    VerifyOrReturnError(chip::SetupPayload::IsValidSetupPIN(passcode), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(disc <= chip::kMaxDiscriminatorValue, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(iterations >= chip::Crypto::kSpake2p_Min_PBKDF_Iterations, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(salt_b64_len > 0, CHIP_ERROR_INVALID_ARGUMENT);

    const size_t salt_len =
        chip::Base64Decode32(salt_b64, static_cast<uint32_t>(salt_b64_len), m_salt);
    VerifyOrReturnError(salt_len >= chip::Crypto::kSpake2p_Min_PBKDF_Salt_Length, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(salt_len <= sizeof(m_salt), CHIP_ERROR_BUFFER_TOO_SMALL);

    m_passcode = passcode;
    m_discriminator = static_cast<uint16_t>(disc);
    m_iterations = iterations;
    m_salt_len = salt_len;
    m_ready = true;

    ESP_LOGI(TAG, "Loaded commissionable data: passcode=%" PRIu32 " discriminator=%u", m_passcode,
             static_cast<unsigned>(m_discriminator));
    return CHIP_NO_ERROR;
}

CHIP_ERROR unique_commissionable_data_provider::GenerateAndPersist()
{
    uint32_t passcode = 0;
    for (int attempt = 0; attempt < 64; ++attempt) {
        uint32_t rnd = 0;
        esp_fill_random(reinterpret_cast<uint8_t *>(&rnd), sizeof(rnd));
        passcode = (rnd % chip::kMaxSetupPasscode) + 1;
        if (chip::SetupPayload::IsValidSetupPIN(passcode)) {
            break;
        }
        passcode = 0;
    }
    VerifyOrReturnError(passcode != 0, CHIP_ERROR_INTERNAL);

    uint16_t disc_rnd = 0;
    esp_fill_random(reinterpret_cast<uint8_t *>(&disc_rnd), sizeof(disc_rnd));
    const uint16_t discriminator =
        static_cast<uint16_t>(disc_rnd & chip::kMaxDiscriminatorValue);

    constexpr uint32_t k_iterations = 1000;
    constexpr size_t k_salt_len = chip::Crypto::kSpake2p_Min_PBKDF_Salt_Length;
    uint8_t salt[k_salt_len] = {};
    esp_fill_random(salt, sizeof(salt));

    char salt_b64[BASE64_ENCODED_LEN(k_salt_len) + 1] = {};
    const uint16_t salt_b64_len =
        chip::Base64Encode(salt, static_cast<uint16_t>(k_salt_len), salt_b64);
    salt_b64[salt_b64_len] = '\0';

    ReturnErrorOnFailure(ESP32Config::WriteConfigValue(ESP32Config::kConfigKey_SetupPinCode, passcode));
    ReturnErrorOnFailure(ESP32Config::WriteConfigValue(ESP32Config::kConfigKey_SetupDiscriminator,
                                                       static_cast<uint32_t>(discriminator)));
    ReturnErrorOnFailure(
        ESP32Config::WriteConfigValue(ESP32Config::kConfigKey_Spake2pIterationCount, k_iterations));
    ReturnErrorOnFailure(ESP32Config::WriteConfigValueStr(ESP32Config::kConfigKey_Spake2pSalt, salt_b64));

    m_passcode = passcode;
    m_discriminator = discriminator;
    m_iterations = k_iterations;
    std::memcpy(m_salt, salt, k_salt_len);
    m_salt_len = k_salt_len;
    m_ready = true;

    ESP_LOGI(TAG, "Generated unique commissionable data: passcode=%" PRIu32 " discriminator=%u",
             m_passcode, static_cast<unsigned>(m_discriminator));
    return CHIP_NO_ERROR;
}

CHIP_ERROR unique_commissionable_data_provider::EnsureReady()
{
    if (m_ready) {
        return CHIP_NO_ERROR;
    }

    if (ESP32Config::ConfigValueExists(ESP32Config::kConfigKey_SetupPinCode)) {
        CHIP_ERROR err = LoadFromNvs();
        if (err == CHIP_NO_ERROR) {
            return CHIP_NO_ERROR;
        }
        ESP_LOGW(TAG, "Invalid stored commissionable data (%" CHIP_ERROR_FORMAT "); regenerating",
                 err.Format());
    }

    return GenerateAndPersist();
}

CHIP_ERROR unique_commissionable_data_provider::GetSetupDiscriminator(uint16_t &setupDiscriminator)
{
    ReturnErrorOnFailure(EnsureReady());
    setupDiscriminator = m_discriminator;
    return CHIP_NO_ERROR;
}

CHIP_ERROR unique_commissionable_data_provider::GetSpake2pIterationCount(uint32_t &iterationCount)
{
    ReturnErrorOnFailure(EnsureReady());
    iterationCount = m_iterations;
    return CHIP_NO_ERROR;
}

CHIP_ERROR unique_commissionable_data_provider::GetSpake2pSalt(chip::MutableByteSpan &saltBuf)
{
    ReturnErrorOnFailure(EnsureReady());
    VerifyOrReturnError(saltBuf.size() >= m_salt_len, CHIP_ERROR_BUFFER_TOO_SMALL);
    std::memcpy(saltBuf.data(), m_salt, m_salt_len);
    saltBuf.reduce_size(m_salt_len);
    return CHIP_NO_ERROR;
}

CHIP_ERROR unique_commissionable_data_provider::GetSpake2pVerifier(chip::MutableByteSpan &verifierBuf,
                                                                  size_t &verifierLen)
{
    ReturnErrorOnFailure(EnsureReady());

    uint8_t salt[chip::Crypto::kSpake2p_Max_PBKDF_Salt_Length] = {};
    chip::MutableByteSpan salt_span(salt);
    ReturnErrorOnFailure(GetSpake2pSalt(salt_span));

    chip::Crypto::Spake2pVerifier verifier;
    ReturnErrorOnFailure(verifier.Generate(m_iterations, salt_span, m_passcode));
    ReturnErrorOnFailure(verifier.Serialize(verifierBuf));
    verifierLen = verifierBuf.size();
    return CHIP_NO_ERROR;
}

CHIP_ERROR unique_commissionable_data_provider::GetSetupPasscode(uint32_t &setupPasscode)
{
    ReturnErrorOnFailure(EnsureReady());
    setupPasscode = m_passcode;
    return CHIP_NO_ERROR;
}
