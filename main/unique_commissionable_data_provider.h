/*
 * Per-device Matter commissionable data (passcode / discriminator / SPAKE2+).
 * Registered via CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER so Test defaults
 * (shared 20202021 / 34970112332) are not used.
 */
#pragma once

#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CHIPError.h>
#include <lib/support/Span.h>
#include <platform/CommissionableDataProvider.h>

class unique_commissionable_data_provider : public chip::DeviceLayer::CommissionableDataProvider {
public:
    unique_commissionable_data_provider() = default;

    CHIP_ERROR GetSetupDiscriminator(uint16_t &setupDiscriminator) override;
    CHIP_ERROR SetSetupDiscriminator(uint16_t setupDiscriminator) override
    {
        (void)setupDiscriminator;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetSpake2pIterationCount(uint32_t &iterationCount) override;
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &saltBuf) override;
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &verifierBuf, size_t &verifierLen) override;

    CHIP_ERROR GetSetupPasscode(uint32_t &setupPasscode) override;
    CHIP_ERROR SetSetupPasscode(uint32_t setupPasscode) override
    {
        (void)setupPasscode;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

private:
    CHIP_ERROR EnsureReady();
    CHIP_ERROR LoadFromNvs();
    CHIP_ERROR GenerateAndPersist();

    bool m_ready = false;
    uint32_t m_passcode = 0;
    uint16_t m_discriminator = 0;
    uint32_t m_iterations = 1000;
    uint8_t m_salt[chip::Crypto::kSpake2p_Max_PBKDF_Salt_Length] = {};
    size_t m_salt_len = 0;
};
