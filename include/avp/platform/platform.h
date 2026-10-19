#pragma once

#include "avp/runtime/boot/boot_policy.h"

#include <stdint.h>


enum {
    AVP_PLATFORM_BASE = 0x00400000,
    /*
     * Virtualization.framework leaves this register at 0 (platform.c), which
     * is also how the ROM tells it from the t8110 machine it was written for.
     */
    AVP_PLATFORM_CHIP_ID = AVP_PLATFORM_BASE + 0x6f0,
    AVP_PLATFORM_VARIANT_CAPACITY = 64,
    AVP_AP_NONCE_SIZE = 32,
    AVP_SEP_NONCE_SIZE = 20,
};

typedef struct {
    uint64_t ecid;
    uint32_t chip_id;
    uint8_t security_domain;
    uint8_t chip_revision;
    uint8_t chip_fuse_mode;
    uint8_t security_epoch;
    uint8_t board_id;
    uint8_t boot_flags;
    char variant[AVP_PLATFORM_VARIANT_CAPACITY];
    uint8_t ap_nonce[AVP_AP_NONCE_SIZE];
    uint8_t sep_nonce[AVP_SEP_NONCE_SIZE];
} AVPPlatformIdentity;

uint64_t platform_ecid(void);
AVPBootPolicy platform_claim_boot_policy(void);
void platform_publish_boot_context(const AVPBootContext *context);
void platform_read_identity(AVPPlatformIdentity *identity);
void platform_progress(uint32_t index, uint32_t value);
void platform_stage(uint32_t value);
