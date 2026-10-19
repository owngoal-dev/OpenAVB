#include "avp/image/boot/boot_image.h"
#include "avp/image/boot/patch_store.h"
#include "avp/image/boot/source_image.h"
#include "avp/crypto/hash/corecrypto_sha512_compat.h"
#include "avp/crypto/hash/sha256.h"
#include "avp/platform/mmio.h"
#include "avp/platform/platform.h"
#include "avp/runtime/memory/runtime_memory.h"
#include "lzfse.h"

size_t lzfse_decode_buffer_with_scratch(
    uint8_t *restrict destination,
    size_t destination_size,
    const uint8_t *restrict source,
    size_t source_size,
    void *restrict scratch
);

__attribute__((weak)) int platform_boot_policy_publish_image_evaluation(
    const AVPBootImageView *image,
    const AVPBootImagePolicy *policy
) {
    (void)image;
    (void)policy;
    return 0;
}


#include "avp/platform/platform_profile.h"

/*
 * Published by start.S when the core was entered at EL2 and read by the
 * handoff. An image that does not ask for EL2 clears it, so the platform
 * profile decides where its payload is entered.
 */
extern volatile uint8_t avp_el2_present;

enum {
    LZFSE_SCRATCH_BASE = 0x70300000,
    LZFSE_SCRATCH_CAPACITY = 0x000c0000,
    EARLY_CONSOLE_DATA_IPA = 0x20010000,
};

static void source_image_diagnostic(const char *message) {
    volatile uint32_t *const data =
        (volatile uint32_t *)EARLY_CONSOLE_DATA_IPA;
    while (*message != '\0') {
        *data = (uint8_t)*message++;
    }
}

static const uint8_t dfu_footer_prefix[] = {
    0xff, 0xff, 0xff, 0xff, 0xac, 0x05,
    0x00, 0x01, 0x55, 0x46, 0x44, 0x10,
};

static const uint8_t zero_crc32[4] = {0, 0, 0, 0};
static uint32_t prepared_size;
static uintptr_t prepared_base;
static uintptr_t prepared_entry;

static bool source_image_publish_policy_metadata(
    const AVPBootImageView *image,
    const AVPBootImagePolicy *policy
) {
    /* SourceIBSS reuses this loader with the weak local no-op. The canonical
     * BootROM supplies the strong policy publication owner. */
    return platform_boot_policy_publish_image_evaluation(image, policy) == 0;
}

static const RuntimeFirebloomType source_image_byte_type = {
    .identity = UINT64_C(0x425954455350414e),
    .element_size = 1,
    .bytewise_copyable = true,
};

static bool bytes_equal(
    const uint8_t *left,
    const uint8_t *right,
    uint32_t size
) {
    for (uint32_t index = 0; index < size; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, uint32_t size) {
    for (uint32_t index = 0; index < size; ++index) {
        destination[index] = source[index];
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, uint32_t size) {
    crc ^= UINT32_MAX;
    for (uint32_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static uint32_t read_u32_le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
        (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 |
        (uint32_t)bytes[3] << 24;
}

bool source_image_measure(
    const uint8_t *prefix,
    uint32_t prefix_size,
    uint32_t *image_size
) {
    if (prefix_size >= sizeof(SourceImageHeader)) {
        const SourceImageHeader *header = (const SourceImageHeader *)prefix;
        if (header->magic == SOURCE_IMAGE_MAGIC &&
            header->version == SOURCE_IMAGE_VERSION &&
            header->header_size == sizeof(*header) &&
            header->image_size >= sizeof(*header) &&
            header->image_size <= SOURCE_IMAGE_CAPACITY) {
            *image_size = header->image_size;
            return true;
        }
    }
    size_t measured_size = 0;
    if (avp_boot_image_measure_der(
            prefix,
            prefix_size,
            &measured_size
        ) != AVP_BOOT_IMAGE_OK ||
        measured_size > SOURCE_IMAGE_CAPACITY) {
        return false;
    }
    *image_size = (uint32_t)measured_size;
    return true;
}

static bool validate_transport(const uint8_t *image, uint32_t downloaded_size) {
    if (downloaded_size < 16) {
        return false;
    }
    const uint8_t *footer = image + downloaded_size - 16;
    if (!bytes_equal(footer, dfu_footer_prefix, sizeof(dfu_footer_prefix))) {
        return false;
    }
    const uint32_t sent = read_u32_le(footer + sizeof(dfu_footer_prefix));
    uint32_t crc = crc32_update(UINT32_MAX, image, downloaded_size - 16);
    crc = crc32_update(crc, footer, sizeof(dfu_footer_prefix));
    /*
     * libirecovery, and so idevicerestore, sends the DFU 1.1 suffix CRC: the
     * register seeded with all ones over the image and the first twelve
     * suffix bytes, not inverted at the end. Apple's ROM takes it, so it is
     * accepted beside the form above.
     */
    uint32_t dfu_crc = crc32_update(0, image, downloaded_size - 16);
    dfu_crc = crc32_update(dfu_crc, footer, sizeof(dfu_footer_prefix)) ^ UINT32_MAX;
    return crc == sent || dfu_crc == sent;
}

static bool prepare_real_ibss(const uint8_t *image, uint32_t image_size) {
    static const uint32_t accepted_types[] = {
        AVP_BOOT_IMAGE_FOURCC('i', 'b', 's', 's'),
    };
    const AVPBootImageDescriptor descriptor = {
        .file_size = image_size,
        .image_size = 0,
        .container = AVP_BOOT_IMAGE_CONTAINER_IMG4,
        .flags = 0,
        .data = {
            .current = (void *)image,
            .lower_bound = (void *)image,
            .upper_bound = (void *)(image + image_size),
            .type = &source_image_byte_type,
        },
    };
    const AVPBootImagePolicy policy = {
        .accepted_image_types = accepted_types,
        .accepted_image_type_count = 1,
        .manifestless_image_type = 0,
        .require_manifest = false,
        .evaluate = 0,
        .context = 0,
    };
    AVPBootImageView evaluated;
    if (avp_boot_image_evaluate(
            &descriptor,
            &policy,
            &evaluated
        ) != AVP_BOOT_IMAGE_OK) {
        source_image_diagnostic("OpenBootROM: iBSS container rejected\n");
        return false;
    }
    source_image_diagnostic("OpenBootROM: iBSS container accepted\n");
    /*
     * What this image is, and what to do to it, both come from the host.
     *
     * The ROM used to hold a profile per build -- the compressed and raw sizes,
     * both digests, the load address, the exception level and the whole patch
     * table -- generated into its source from the platform JSON.  Every one of
     * those is a property of one particular payload, so the ROM could boot that
     * payload and nothing else, and rebuilding the payload meant rebuilding the
     * ROM.  None of it is compiled in any more: the description is uploaded
     * before the image is, and an image nobody described is refused here.
     *
     * The measurements did not go away with the profile.  The image is still
     * checked compressed and again raw, and still refused when it is not the
     * one described; what changed is only where the description comes from.
     */
    const AVPPatchStageHeader *told = patch_store_identity(PATCH_STAGE_IBSS);
    uint32_t patch_count = 0;
    const AVPImagePatch *patches = patch_store_patches(
        PATCH_STAGE_IBSS,
        &patch_count
    );
    uint8_t digest[32];

    if (told == 0) {
        source_image_diagnostic(
            "OpenBootROM: no image description was uploaded\n");
        return false;
    }
    sha256(evaluated.payload_bytes, evaluated.payload_size, digest);
    if (evaluated.payload_size != told->compressed_size ||
        !bytes_equal(digest, told->compressed_digest, sizeof(digest)) ||
        lzfse_decode_scratch_size() > LZFSE_SCRATCH_CAPACITY) {
        source_image_diagnostic("OpenBootROM: iBSS compressed identity rejected\n");
        return false;
    }
    source_image_diagnostic("OpenBootROM: iBSS compressed identity accepted\n");
    const uintptr_t load_base = (uintptr_t)told->load_base;
    const size_t decoded = lzfse_decode_buffer_with_scratch(
        (uint8_t *)load_base,
        told->raw_size,
        evaluated.payload_bytes,
        told->compressed_size,
        (void *)LZFSE_SCRATCH_BASE
    );
    if (decoded != told->raw_size) {
        source_image_diagnostic("OpenBootROM: iBSS decompression rejected\n");
        return false;
    }
    source_image_diagnostic("OpenBootROM: iBSS decompressed\n");
    sha256((const uint8_t *)load_base, told->raw_size, digest);
    if (!bytes_equal(digest, told->raw_digest, sizeof(digest))) {
        source_image_diagnostic("OpenBootROM: iBSS raw identity rejected\n");
        return false;
    }
    source_image_diagnostic("OpenBootROM: iBSS raw identity accepted\n");
    if (patch_count != 0 && patches != 0) {
        uint32_t *instructions = (uint32_t *)load_base;

        for (uint32_t index = 0; index < patch_count; ++index) {
            instructions[patches[index].offset / sizeof(uint32_t)] =
                patches[index].instruction;
        }
        source_image_diagnostic("OpenBootROM: image patches applied\n");
    }
    if (told->entry_exception_level != 2) {
        avp_el2_present = 0;
    }
    if (!source_image_publish_policy_metadata(&evaluated, &policy)) {
        return false;
    }
    prepared_size = told->raw_size;
    prepared_base = load_base;
    prepared_entry = load_base;
    return true;
}

static bool prepare_source_fixture(const uint8_t *image, uint32_t image_size) {
    const SourceImageHeader *header = (const SourceImageHeader *)image;
    if (image_size < sizeof(*header) ||
        header->magic != SOURCE_IMAGE_MAGIC ||
        header->version != SOURCE_IMAGE_VERSION ||
        header->header_size != sizeof(*header) ||
        header->image_size != image_size ||
        header->identity != SOURCE_IMAGE_IDENTITY ||
        header->content_crc32 == 0 || header->reserved != 0 ||
        header->entry_offset < header->header_size ||
        header->entry_offset >= header->image_size ||
        (header->entry_offset & 3) != 0) {
        return false;
    }
    uint32_t content_crc = crc32_update(0, image, 24);
    content_crc = crc32_update(content_crc, zero_crc32, sizeof(zero_crc32));
    content_crc = crc32_update(content_crc, image + 28, image_size - 28);
    if (content_crc != header->content_crc32) {
        return false;
    }
    copy_bytes((uint8_t *)SOURCE_IMAGE_BASE, image, image_size);
    const AVPBootImageView evaluated = {
        .container = AVP_BOOT_IMAGE_CONTAINER_MEMZ,
        .image_type = AVP_BOOT_IMAGE_FOURCC('i', 'b', 's', 's'),
    };
    const AVPBootImagePolicy policy = {0};
    if (!source_image_publish_policy_metadata(&evaluated, &policy)) {
        return false;
    }
    prepared_size = image_size;
    prepared_base = SOURCE_IMAGE_BASE;
    prepared_entry = SOURCE_IMAGE_BASE + header->entry_offset;
    return true;
}

/* Publish the boot manifest hash, because on this chain nothing else can.
 *
 * iBoot reads a 48-byte digest out of `0x2_3D2E_0000` and hex-encodes it into
 * `<prefix>/<96 hex>/LocalPolicy.cryptex1.img4`, the Preboot directory a
 * restore names after the same value.  With the window empty the getter behind
 * it answers -1 and `lib/localpolicy/splat/localpolicy_splat.c:72` panics.
 *
 * The block is a pair of windows: a stage writes its manifest digest to
 * `+0x44`, the 4CC of the image it is handing on to `+0x74`, and locks the lot
 * by setting bit 16 of `+0x84`; the next stage reads `+0x00`, whose presence
 * bit is bit 0 of `+0x40`.  Measured on a DFU round, iBSS does exactly that --
 * 48 bytes then `'ibec'` -- so the firmware holds up its half of the contract.
 * A local boot has no firmware stage before LLB, only this ROM, and the
 * hardware carry between the windows is modelled in the machine.  So this is
 * the missing producer, not a workaround.
 *
 * The value is **SHA-384 over the manifest**, verified byte for byte against
 * the Preboot directory name of a finished 21A329 restore.  Nothing here is
 * per-device or per-disk: whatever verified the image can compute it, and this
 * ROM just did.
 *
 * `+0x44` is written but never seeded from outside -- iBoot locks it itself and
 * panics on finding it already locked -- so this runs once, here, before the
 * firmware starts.  Bit 16 of `+0x40` is the factory lock on the ROM manifest
 * and is not ours to touch; the board seeds it.
 *
 * Only the t8110 machine has the block.  Virtualization.framework maps nothing
 * at `0x2_3D2E_0000` unless the guest has about 7.5 GiB of memory or more:
 * below that the first read of `+0x84` aborts and the ROM parks in pvpanic
 * before LLB runs, and above it the window is guest RAM.  Its iBoot boots on
 * Apple's ROM, which publishes nothing here, so there the ROM leaves it alone.
 */
enum {
    SOURCE_IMAGE_SIDP_BOOT = 0x23d2e0044ull,
    SOURCE_IMAGE_SIDP_BOOT_FOURCC = 0x23d2e0074ull,
    SOURCE_IMAGE_SIDP_BOOT_FLAGS = 0x23d2e0084ull,
    SOURCE_IMAGE_SIDP_DIGEST = 48,
    SOURCE_IMAGE_SIDP_PRESENT = 1u << 0,
    SOURCE_IMAGE_SIDP_LOCKED = 1u << 16,
};

static void source_image_publish_boot_manifest(
    const AVPBootImageView *image,
    uint32_t next_fourcc
) {
    AVPCCSHA512Context context;
    uint8_t digest[AVP_SHA384_DIGEST_SIZE];

    if (mmio_read32(AVP_PLATFORM_CHIP_ID) == 0) {
        return;
    }
    if (image->encoded_manifest_bytes == 0 ||
        image->encoded_manifest_size == 0) {
        source_image_diagnostic(
            "OpenBootROM: iLLB carries no manifest to measure\n");
        return;
    }
    if ((mmio_read32(SOURCE_IMAGE_SIDP_BOOT_FLAGS) &
         SOURCE_IMAGE_SIDP_LOCKED) != 0) {
        return;                         /* someone already spoke for this boot */
    }
    avp_ccsha512_init(&context, &avp_ccsha384_info);
    if (avp_ccsha512_update(&context, image->encoded_manifest_bytes,
                            image->encoded_manifest_size) != 0 ||
        ccsha512_final(&avp_ccsha384_info, &context, digest) != 0) {
        source_image_diagnostic("OpenBootROM: iLLB manifest would not hash\n");
        return;
    }
    for (size_t at = 0; at < SOURCE_IMAGE_SIDP_DIGEST; at += 4) {
        mmio_write32(
            SOURCE_IMAGE_SIDP_BOOT + at,
            (uint32_t)digest[at] |
            ((uint32_t)digest[at + 1] << 8) |
            ((uint32_t)digest[at + 2] << 16) |
            ((uint32_t)digest[at + 3] << 24)
        );
    }
    mmio_write32(SOURCE_IMAGE_SIDP_BOOT_FOURCC, next_fourcc);
    mmio_write32(
        SOURCE_IMAGE_SIDP_BOOT_FLAGS,
        mmio_read32(SOURCE_IMAGE_SIDP_BOOT_FLAGS) |
        SOURCE_IMAGE_SIDP_PRESENT
    );
    mmio_write32(
        SOURCE_IMAGE_SIDP_BOOT_FLAGS,
        mmio_read32(SOURCE_IMAGE_SIDP_BOOT_FLAGS) |
        SOURCE_IMAGE_SIDP_LOCKED
    );
    source_image_diagnostic("OpenBootROM: iLLB boot manifest published\n");
}


static bool prepare_virtual_firmware_illb(
    const uint8_t *image,
    uint32_t image_size
) {
    const uint32_t resident_capacity =
        VIRTUAL_FIRMWARE_RESIDENT_LIMIT - VIRTUAL_FIRMWARE_IMAGE_BASE;
    static const uint32_t accepted_types[] = {
        AVP_BOOT_IMAGE_FOURCC('i', 'l', 'l', 'b'),
    };
    const AVPBootImageDescriptor descriptor = {
        .file_size = image_size,
        .image_size = 0,
        .container = AVP_BOOT_IMAGE_CONTAINER_IMG4,
        .flags = 0,
        .data = {
            .current = (void *)image,
            .lower_bound = (void *)image,
            .upper_bound = (void *)(image + image_size),
            .type = &source_image_byte_type,
        },
    };
    const AVPBootImagePolicy policy = {
        .accepted_image_types = accepted_types,
        .accepted_image_type_count = 1,
        .manifestless_image_type = 0,
        .require_manifest = true,
        .evaluate = 0,
        .context = 0,
    };
    AVPBootImageView evaluated;
    if (avp_boot_image_evaluate(&descriptor, &policy, &evaluated) !=
            AVP_BOOT_IMAGE_OK) {
        source_image_diagnostic("OpenBootROM: iLLB container rejected\n");
        return false;
    }
    source_image_diagnostic("OpenBootROM: iLLB container accepted\n");
    source_image_publish_boot_manifest(
        &evaluated,
        AVP_BOOT_IMAGE_FOURCC('i', 'l', 'l', 'b')
    );
    /* A stock t8110 LLB arrives lzfse-wrapped and decodes to two and a half
     * times the resident window; its prologue self-describes 0x1fc050000 as
     * the link base, so decode straight into the SRAM aperture there.
     * Plaintext payloads keep the resident path below. */
    if (evaluated.payload_size >= 4 &&
        bytes_equal(evaluated.payload_bytes, (const uint8_t *)"bvx2", 4)) {
        if (lzfse_decode_scratch_size() > LZFSE_SCRATCH_CAPACITY) {
            source_image_diagnostic(
                "OpenBootROM: iLLB decompression rejected\n");
            return false;
        }
        runtime_bzero(
            (void *)VIRTUAL_FIRMWARE_SRAM_IMAGE_BASE,
            VIRTUAL_FIRMWARE_SRAM_IMAGE_CAPACITY
        );
        source_image_diagnostic("OpenBootROM: iLLB SRAM resident cleared\n");
        const size_t decoded = lzfse_decode_buffer_with_scratch(
            (uint8_t *)VIRTUAL_FIRMWARE_SRAM_IMAGE_BASE,
            VIRTUAL_FIRMWARE_SRAM_IMAGE_CAPACITY,
            evaluated.payload_bytes,
            evaluated.payload_size,
            (void *)LZFSE_SCRATCH_BASE
        );
        if (decoded == 0 ||
            decoded == VIRTUAL_FIRMWARE_SRAM_IMAGE_CAPACITY) {
            source_image_diagnostic(
                "OpenBootROM: iLLB decompression rejected\n");
            return false;
        }
        source_image_diagnostic("OpenBootROM: iLLB decompressed\n");
        /*
         * The words this build cannot execute here, rewritten before it runs.
         *
         * On this board the four stages of the firmware are one binary: for
         * 19A348 iBSS, iBEC, LLB and iBoot are the same 2134448 bytes, sha256
         * b73a4846..., and the table the host worked out by scanning iBSS
         * describes this image exactly -- including its load base, which the
         * scanner reports as 0x1fc050000, the address this decodes to.  So the
         * table uploaded for the recovery chain is the table the installed
         * chain needs, and it is used as it stands.
         *
         * Without it LLB stops in the first of the four analogue calibrations:
         * "calibrate_ca: Memory CA calibration: Unable to find any Vref which
         * did not panic for channel 0".  There is no analogue channel here to
         * find an eye in -- a modelled DRAM answers every setting the same --
         * so the four calls are dropped.  The same table diverts LLB's handoff
         * back into this ROM, which is what lets the iBoot it hands to be
         * patched in turn, by the code that already patches iBEC.
         *
         * Nothing uploads a table on the installed chain -- there is no host
         * on the other end of the recovery interface -- so it is put in the
         * store, which is a fixed window in main RAM, before the machine
         * starts.  An absent table is not an error: a plaintext or already
         * patched LLB boots with none.
         */
        uint32_t llb_patch_count = 0;
        const AVPImagePatch *llb_patches =
            patch_store_patches(PATCH_STAGE_IBSS, &llb_patch_count);

        if (llb_patches != 0 && llb_patch_count != 0) {
            uint32_t *instructions =
                (uint32_t *)VIRTUAL_FIRMWARE_SRAM_IMAGE_BASE;
            uint32_t applied = 0;

            for (uint32_t index = 0; index < llb_patch_count; ++index) {
                const uint32_t offset = llb_patches[index].offset;

                if (offset + sizeof(uint32_t) > decoded || (offset & 3) != 0) {
                    continue;
                }
                instructions[offset / sizeof(uint32_t)] =
                    llb_patches[index].instruction;
                ++applied;
            }
            if (applied == llb_patch_count) {
                source_image_diagnostic("OpenBootROM: iLLB patches applied\n");
            } else {
                source_image_diagnostic(
                    "OpenBootROM: iLLB patches partly outside the image\n");
            }
        }
        if (!source_image_publish_policy_metadata(&evaluated, &policy)) {
            return false;
        }
        prepared_size = (uint32_t)((decoded + 63) & ~(size_t)63);
        prepared_base = VIRTUAL_FIRMWARE_SRAM_IMAGE_BASE;
        prepared_entry = VIRTUAL_FIRMWARE_SRAM_IMAGE_BASE;
        return true;
    }
    if (evaluated.payload_size == 0 ||
        evaluated.payload_size > resident_capacity) {
        source_image_diagnostic("OpenBootROM: iLLB payload rejected\n");
        return false;
    }
    /* The file-backed payload ends before the resident data boundary. Clearing
     * this complete span establishes the LLB BSS while preserving live source
     * page tables and stacks used to finalize SEP before handoff. */
    runtime_bzero(
        (void *)VIRTUAL_FIRMWARE_IMAGE_BASE,
        resident_capacity
    );
    source_image_diagnostic("OpenBootROM: iLLB resident cleared\n");
    copy_bytes(
        (uint8_t *)VIRTUAL_FIRMWARE_IMAGE_BASE,
        evaluated.payload_bytes,
        (uint32_t)evaluated.payload_size
    );
    if (!source_image_publish_policy_metadata(&evaluated, &policy)) {
        return false;
    }
    /* The payload owns this complete resident range. Cache synchronization
     * must include the zeroed BSS tail before the handoff disables EL1 data
     * caching, otherwise the next stage observes stale physical memory. */
    prepared_size = resident_capacity;
    prepared_base = VIRTUAL_FIRMWARE_IMAGE_BASE;
    prepared_entry = VIRTUAL_FIRMWARE_IMAGE_BASE;
    return true;
}

bool source_image_validate(uint32_t downloaded_size) {
    const uint8_t *image = (const uint8_t *)SOURCE_IMAGE_UPLOAD_BASE;
    prepared_size = 0;
    prepared_base = 0;
    prepared_entry = 0;
    if (downloaded_size > SOURCE_IMAGE_CAPACITY ||
        !validate_transport(image, downloaded_size)) {
        return false;
    }
    const uint32_t image_size = downloaded_size - 16;
    return prepare_source_fixture(image, image_size) ||
        prepare_real_ibss(image, image_size);
}

bool source_image_prepare_virtual_firmware(uint32_t image_size) {
    const uint8_t *image = (const uint8_t *)SOURCE_IMAGE_UPLOAD_BASE;
    prepared_size = 0;
    prepared_base = 0;
    prepared_entry = 0;
    if (image_size == 0 || image_size > SOURCE_IMAGE_CAPACITY) {
        return false;
    }
    return prepare_virtual_firmware_illb(image, image_size);
}

uint32_t source_image_size(void) {
    return prepared_size;
}

uintptr_t source_image_base(void) {
    return prepared_base;
}

uintptr_t source_image_entry(void) {
    return prepared_entry;
}
