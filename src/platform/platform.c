#include "avp/platform/mmio.h"
#include "avp/platform/platform.h"


enum {
    PLATFORM_BOARD_ID_OFFSET = 0x10,
    PLATFORM_CHIP_REVISION_OFFSET = 0x14,
    PLATFORM_ECID_LOW_OFFSET = 0x18,
    PLATFORM_ECID_HIGH_OFFSET = 0x1c,
    PLATFORM_SECURITY_DOMAIN_OFFSET = 0x2c,
    PLATFORM_BOOT_CONFIGURATION_OFFSET = 0x30,
    PLATFORM_BOOT_OPTIONS_OFFSET = 0x34,
    PLATFORM_ENTROPY_OFFSET = 0x38,
    PLATFORM_BOOT_CONTEXT_OFFSET = 0x300,
    PLATFORM_VARIANT_OFFSET = 0x5c8,
    /*
     * The matching vphone600 machine profile carries these values outside
     * the readable identity registers.
     */
    VPHONE600_CHIP_FUSE_MODE = 3,
    VPHONE600_SECURITY_EPOCH = 1,
    VPHONE600_BOOT_FLAGS = 0x3c,
    /*
     * Virtualization.framework leaves the chip ID register at 0; Apple's ROM
     * names the chip itself. Without it DFU enumerates as CPID:0000 and no
     * host can tell which device it is.
     */
    VPHONE600_CHIP_ID = 0xfe01,
};

static void read_variant(char output[AVP_PLATFORM_VARIANT_CAPACITY]) {
    for (uint32_t offset = 0;
         offset < AVP_PLATFORM_VARIANT_CAPACITY;
         offset += sizeof(uint32_t)) {
        const uint32_t word = mmio_read32(
            AVP_PLATFORM_BASE + PLATFORM_VARIANT_OFFSET + offset
        );
        for (uint32_t byte = 0; byte < sizeof(uint32_t); ++byte) {
            output[offset + byte] = (char)(uint8_t)(word >> (byte * 8));
        }
    }
    output[AVP_PLATFORM_VARIANT_CAPACITY - 1] = '\0';
}

uint64_t platform_ecid(void) {
    const uint64_t low = mmio_read32(
        AVP_PLATFORM_BASE + PLATFORM_ECID_LOW_OFFSET
    );
    const uint64_t high = mmio_read32(
        AVP_PLATFORM_BASE + PLATFORM_ECID_HIGH_OFFSET
    );
    return low | (high << 32);
}

AVPBootPolicy platform_claim_boot_policy(void) {
    const uint32_t configuration = mmio_read32(
        AVP_PLATFORM_BASE + PLATFORM_BOOT_CONFIGURATION_OFFSET
    );
    const uint32_t options = mmio_read32(
        AVP_PLATFORM_BASE + PLATFORM_BOOT_OPTIONS_OFFSET
    );
    const AVPBootPolicy policy = boot_policy_decode(configuration, options);
    if (policy.selection == AVP_BOOT_SELECTION_FORCE_DFU) {
        mmio_write32(
            AVP_PLATFORM_BASE + PLATFORM_BOOT_OPTIONS_OFFSET,
            options & ~1u
        );
    }
    return policy;
}

void platform_publish_boot_context(const AVPBootContext *context) {
    const uint32_t identity_word = context->status |
        (uint32_t)context->configuration << 8 |
        (uint32_t)context->board_id << 16 |
        (uint32_t)context->security_epoch << 24;
    mmio_write32(
        AVP_PLATFORM_BASE + PLATFORM_BOOT_CONTEXT_OFFSET,
        identity_word
    );
    for (uint32_t offset = sizeof(uint32_t);
         offset < sizeof(*context);
         offset += sizeof(uint32_t)) {
        mmio_write32(
            AVP_PLATFORM_BASE + PLATFORM_BOOT_CONTEXT_OFFSET + offset,
            0
        );
    }
}

void platform_read_identity(AVPPlatformIdentity *identity) {
    identity->ecid = platform_ecid();
    identity->chip_id = mmio_read32(AVP_PLATFORM_CHIP_ID);
    if (identity->chip_id == 0) {
        identity->chip_id = VPHONE600_CHIP_ID;
    }
    identity->security_domain = (uint8_t)mmio_read32(
        AVP_PLATFORM_BASE + PLATFORM_SECURITY_DOMAIN_OFFSET
    );
    identity->chip_revision = (uint8_t)mmio_read32(
        AVP_PLATFORM_BASE + PLATFORM_CHIP_REVISION_OFFSET
    );
    identity->chip_fuse_mode = VPHONE600_CHIP_FUSE_MODE;
    identity->security_epoch = VPHONE600_SECURITY_EPOCH;
    identity->board_id = (uint8_t)mmio_read32(
        AVP_PLATFORM_BASE + PLATFORM_BOARD_ID_OFFSET
    );
    identity->boot_flags = VPHONE600_BOOT_FLAGS;
    read_variant(identity->variant);
    for (uint32_t index = 0; index < sizeof(identity->ap_nonce); ++index) {
        identity->ap_nonce[index] = 0;
    }
    for (uint32_t index = 0; index < sizeof(identity->sep_nonce); ++index) {
        identity->sep_nonce[index] = 0;
    }
}

void platform_progress(uint32_t index, uint32_t value) {
    mmio_write32(AVP_PLATFORM_BASE + 0x64 + index * 4, value);
}

void platform_stage(uint32_t value) {
    mmio_write32(AVP_PLATFORM_BASE + 0x3ac, value);
}
