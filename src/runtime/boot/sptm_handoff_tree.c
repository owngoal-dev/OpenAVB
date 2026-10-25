/*
 * Device tree edits at the SPTM handoff.
 *
 * The handoff runs with the MMU off, every address physical and the caches
 * already clean -- a good place to change the tree, and the only one that
 * happens after iBoot has finished publishing it.  What it is not is a good
 * place for assembly: an Apple device tree is walked, not indexed, so finding
 * a node means counting properties and recursing through children, and the
 * one assumption that makes it look easy in a register-only routine -- that a
 * node's name is its first property -- is false.  /chosen/manifest-properties
 * keeps its name thirty-third of fifty-two, so a routine that reads the node
 * header at a fixed distance behind the name edits somebody else's data and
 * leaves a tree that no longer walks.
 *
 * So the handoff lends this a stack out of the ROM aperture and calls in.
 *
 * Nothing here may touch .data or .bss: those are linked to run from DRAM the
 * runtime has long since given up.  Locals and string literals only.  And no
 * access wider than the four bytes a property is aligned to -- with the MMU
 * off the memory is Device, where an unaligned load or store aborts.
 */
#include <avp/platform/platform_profile.h>
#include "avp/image/boot/patch_store.h"
#include <stdint.h>

/*
 * The stack guard lives in .data, which is linked to DRAM the runtime has
 * given up by the time any of this runs; reading it would fault.
 */
#define HANDOFF __attribute__((no_stack_protector))

#define ADT_NAME_LEN    32u
#define ADT_PROP_HEADER 36u

HANDOFF static uint32_t adt_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

HANDOFF static void adt_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The top bit marks a placeholder the loader was meant to fill in. */
HANDOFF static uint32_t adt_prop_size(const uint8_t *prop)
{
    return adt_rd32(prop + ADT_NAME_LEN) & 0x7fffffffu;
}

HANDOFF static uint8_t *adt_prop_next(uint8_t *prop)
{
    return prop + ADT_PROP_HEADER + ((adt_prop_size(prop) + 3u) & ~3u);
}

HANDOFF static int adt_name_is(const uint8_t *prop, const char *name)
{
    uint32_t i = 0;

    while (i < ADT_NAME_LEN && name[i] != '\0') {
        if (prop[i] != (uint8_t)name[i]) {
            return 0;
        }
        i++;
    }
    return i < ADT_NAME_LEN && prop[i] == 0;
}

HANDOFF static int adt_name_starts(const uint8_t *prop, const char *name)
{
    uint32_t i = 0;

    while (name[i] != '\0') {
        if (i >= ADT_NAME_LEN || prop[i] != (uint8_t)name[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

HANDOFF static uint8_t *adt_prop(uint8_t *node, const char *name)
{
    uint32_t count = adt_rd32(node);
    uint8_t *prop = node + 8;

    for (uint32_t i = 0; i < count; i++) {
        if (adt_name_is(prop, name)) {
            return prop;
        }
        prop = adt_prop_next(prop);
    }
    return 0;
}

HANDOFF static uint8_t *adt_after_props(uint8_t *node)
{
    uint32_t count = adt_rd32(node);
    uint8_t *prop = node + 8;

    for (uint32_t i = 0; i < count; i++) {
        prop = adt_prop_next(prop);
    }
    return prop;
}

HANDOFF static uint8_t *adt_node_end(uint8_t *node)
{
    uint32_t children = adt_rd32(node + 4);
    uint8_t *p = adt_after_props(node);

    for (uint32_t i = 0; i < children; i++) {
        p = adt_node_end(p);
    }
    return p;
}

HANDOFF static int adt_node_is(uint8_t *node, const char *name)
{
    uint8_t *prop = adt_prop(node, "name");
    uint32_t size;
    uint32_t i = 0;

    if (prop == 0) {
        return 0;
    }
    size = adt_prop_size(prop);
    while (name[i] != '\0') {
        if (i >= size || prop[ADT_PROP_HEADER + i] != (uint8_t)name[i]) {
            return 0;
        }
        i++;
    }
    return i >= size || prop[ADT_PROP_HEADER + i] == 0;
}

HANDOFF static uint8_t *adt_child(uint8_t *node, const char *name)
{
    uint32_t children = adt_rd32(node + 4);
    uint8_t *p = adt_after_props(node);

    for (uint32_t i = 0; i < children; i++) {
        if (adt_node_is(p, name)) {
            return p;
        }
        p = adt_node_end(p);
    }
    return 0;
}

/*
 * Something to say, out of the same hole the ROM's own diagnostics go through.
 *
 * boot_diagnostic writes bytes one at a time to a fixed physical address, so
 * it works here for the same reason everything else does: the address is the
 * physical one whether or not there is a translation regime, and a four-byte
 * store to Device memory is legal where a wider or unaligned one is not.  It
 * is the only way this code can be seen at all -- the machine is between two
 * firmwares, iBoot's console is gone and the kernel's does not exist yet.
 */
#define HANDOFF_CONSOLE 0x20010000ull

HANDOFF static void say(const char *message)
{
    volatile uint32_t *const data = (volatile uint32_t *)HANDOFF_CONSOLE;

    while (*message != '\0') {
        *data = (uint32_t)(uint8_t)*message++;
    }
}

HANDOFF static void say_hex(uint64_t value)
{
    volatile uint32_t *const data = (volatile uint32_t *)HANDOFF_CONSOLE;
    int shift;

    *data = (uint32_t)'0';
    *data = (uint32_t)'x';
    for (shift = 60; shift >= 0; shift -= 4) {
        unsigned digit = (unsigned)((value >> shift) & 0xfu);

        *data = (uint32_t)(digit < 10u ? '0' + digit : 'a' + digit - 10u);
    }
}

HANDOFF static void adt_clean(const uint8_t *from, const uint8_t *to)
{
    uint64_t a = (uint64_t)from & ~63ull;

    while (a < (uint64_t)to) {
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        a += 64;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Publish lp-smb0 and lp-smb1 under /chosen/asmb.
 *
 * The monitor's trusted execution half counts how many of those two exist to
 * decide the secure boot level -- both is the permissive chip, one is the
 * reduced chip, neither leaves it at zero, and at zero it insists on a boot
 * nonce hash that iBoot never published and panics.  iBoot creates the node
 * and puts nothing in it, because on real hardware those come from a local
 * policy and this restore has no such component.
 *
 * The tree cannot grow -- past its end is not mapped, and a store there
 * aborts -- so the eighty bytes for two records come from inside it:
 * manifest-properties carries a pool of placeholders nobody reads, and two
 * adjacent four-byte ones are exactly two records' worth.  Drop that pair,
 * slide what lies between them and the asmb node down, and the tree ends
 * where it ended.
 */
/*
 * Let the kernel run beside a monitor of a different build.
 *
 * arm_init reads the variant SPTM published and panics when it disagrees with
 * its own -- "Mismatch between xnu variant (%s) and SPTM variant (0x%x)", at
 * arm_init_sptm.c:899.  The release kernelcache wants that word to be zero and
 * the monitor we run is the development one, so it never gets past machine
 * init: it calls panic before any console exists, panic enters the debugger
 * with XNU's own undefined instruction, and the nested panic halts without
 * printing a word.
 *
 * The kernelcache cannot be swapped for a matching one -- the ticket names its
 * digest, and iBoot answers "Kernelcache image not valid" -- and it cannot be
 * edited on disk for the same reason.  What it can be is edited here, after
 * iBoot has checked it and before it runs, which is the same standing this
 * code already has with the device tree.
 *
 * The check is found by its instructions rather than its address, so a
 * different build moves it without this having to be told.  If that shape is
 * not there, nothing is written.
 */
static void kc_relax_variant_check(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark,
    uint8_t *clock_mark,
    uint8_t *counts
);
static void kc_force_root_hash_disabled(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark
);
HANDOFF static void kc_skip_rootvp_panic(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark
);
static void kc_force_root_live_fs(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark
);
static void kc_force_img4_firmware_ok(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark
);
static void patch_store_apply(uint32_t stage, uint8_t *base);
static void patch_store_apply_txm(uint32_t stage, uint8_t *txm);

void sptm_handoff_publish_lp_smb(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_publish_lp_smb(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 56);
    uint8_t *chosen;
    uint8_t *asmb;
    uint8_t *pool;
    uint8_t *first = 0;
    uint8_t *previous = 0;
    uint8_t *prop;
    uint8_t *insert;
    uint8_t *record;
    uint32_t count;
    uint32_t i;

    adt_wr32(mark, 0xa5c0);
    chosen = adt_child(root, "chosen");
    if (chosen == 0) {
        return;
    }
    asmb = adt_child(chosen, "asmb");
    pool = adt_child(chosen, "manifest-properties");
    if (asmb == 0 || pool == 0) {
        adt_wr32(mark, 0xa5c2);
        return;
    }
    if (adt_prop(asmb, "lp-smb0") != 0) {
        adt_wr32(mark, 0xa5c3);
        return;
    }

    count = adt_rd32(pool);
    prop = pool + 8;
    for (i = 0; i < count; i++) {
        int spare = adt_prop_size(prop) == 4 && adt_name_is(prop, "") == 0 &&
                    prop[0] == 'U' && prop[1] == 'n' && prop[2] == 'u' &&
                    prop[3] == 's';

        if (spare && previous != 0 && prop == previous + 40) {
            first = previous;
            break;
        }
        previous = spare ? prop : 0;
        prop = adt_prop_next(prop);
    }
    if (first == 0) {
        adt_wr32(mark, 0xa5c4);
        return;
    }

    prop = adt_prop(asmb, "name");
    if (prop == 0) {
        adt_wr32(mark, 0xa5c5);
        return;
    }
    insert = adt_prop_next(prop);
    if (insert < first + 80) {
        adt_wr32(mark, 0xa5c6);      /* the donor has to come first */
        return;
    }

    adt_wr32(pool, count - 2);
    for (uint8_t *s = first + 80, *d = first; s < insert; s++, d++) {
        *d = *s;
    }
    record = insert - 80;
    for (i = 0; i < 80u; i++) {
        record[i] = 0;
    }
    record[0] = 'l'; record[1] = 'p'; record[2] = '-';
    record[3] = 's'; record[4] = 'm'; record[5] = 'b'; record[6] = '0';
    adt_wr32(record + 32, 4);
    adt_wr32(record + 36, 1);
    record[40] = 'l'; record[41] = 'p'; record[42] = '-';
    record[43] = 's'; record[44] = 'm'; record[45] = 'b'; record[46] = '1';
    adt_wr32(record + 72, 4);
    adt_wr32(record + 76, 1);

    /* The node moved with everything else between the pool and here. */
    adt_wr32(asmb - 80, 3);

    adt_clean(first, insert);
    adt_wr32(mark, 0xa5c1);
    adt_wr32(mark + 8, (uint32_t)(uint64_t)record);
}

/*
 * The kernel's own text, patched where iBoot left it.
 *
 * /chosen/memory-map carries where iBoot put the kernelcache's executable
 * segment, as a physical address and a length, which is all the search needs.
 *
 * This used to hang off the tail of publish_lp_smb, on the reasoning that the
 * tree was already open.  It is its own step now: lp-smb is for the monitor's
 * trusted execution half and needs an asmb node and a donor property to carve
 * space out of, and a tree that has neither -- 15.0.1's, which has no monitor
 * to tell anything -- returned before ever reaching the kernel.  The two have
 * nothing to do with each other beyond both wanting /chosen.
 *
 * Breadcrumb at scratch + 84: 0xa5b0 on entry, 0xa5b1 if the map names no
 * kernel, 0xa5b2 once the patches have been applied.  scratch + 128 and
 * scratch + 132 count the ACNTVCT_EL0 reads rewritten to CNTVCT_EL0, in
 * Kernel-__TEXT_EXEC and in Kernel-__PPLTEXT: 19A348 has 92 and none.
 *
 * And how often each kc_patches row matched, per range: scratch + 0x400 for
 * the kernel text and scratch + 0x480 for PPL.  The first word is
 * 0x4b430000 | the number of rows ("KC"), then one word per row in table
 * order.  The bitmask in the mark word says only whether a row hit; a row
 * that hits twice where one site was meant looks the same there, and that is
 * exactly what an encoding-matched rewrite has to rule out.  A host that can
 * read guest memory (a debugger or the emulator's monitor) reads both blocks
 * and names the rows.
 */
#define KC_COUNTS_KERNEL 0x400u
#define KC_COUNTS_PPL    0x480u
/*
 * Where iBoot put the kernel's executable text, as a physical address and a
 * length.
 *
 * The two firmwares name it differently and neither name is a guess: 17.0's
 * iBoot publishes the whole read-execute half of the kernelcache as one entry
 * called BootKC-rx, and 15.0.1's publishes the Mach-O's segments one at a time
 * -- Kernel-__TEXT, Kernel-__TEXT_EXEC, Kernel-__DATA and the rest -- with the
 * code in __TEXT_EXEC.  Both were read out of a live tree; the placeholders the
 * uploaded tree ships, MemoryMapReserved-0 through -15, are what iBoot renames
 * as it fills them in, so nothing about the names can be seen before the boot.
 *
 * Looking for BootKC-rx alone is what made the 15.0.1 handoff quietly do
 * nothing: no entry, no range, no patches, and -- because the same range is
 * what says whether the kernel or a monitor comes next -- the trap for a
 * guarded ERET armed for a monitor that was never there.
 */
HANDOFF static int map_range(
    uint8_t *root,
    const char *name,
    uint64_t *at,
    uint64_t *len
)
{
    uint8_t *chosen = adt_child(root, "chosen");
    uint8_t *map = chosen != 0 ? adt_child(chosen, "memory-map") : 0;
    uint8_t *entry = map != 0 ? adt_prop(map, name) : 0;
    unsigned k;

    *at = 0;
    *len = 0;
    if (entry == 0 || adt_prop_size(entry) < 16) {
        return 0;
    }
    for (k = 0; k < 8; k++) {
        *at |= (uint64_t)entry[ADT_PROP_HEADER + k] << (k * 8);
        *len |= (uint64_t)entry[ADT_PROP_HEADER + 8 + k] << (k * 8);
    }
    return *at != 0 && *len != 0;
}

HANDOFF static int kc_text_range(uint8_t *root, uint64_t *at, uint64_t *len)
{
    return map_range(root, "BootKC-rx", at, len) ||
           map_range(root, "Kernel-__TEXT_EXEC", at, len);
}

void sptm_handoff_kernel_patch(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_kernel_patch(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 84);
    uint64_t at = 0;
    uint64_t len = 0;

    adt_wr32(mark, 0xa5b0);
    if (kc_text_range(root, &at, &len) == 0) {
        adt_wr32(mark, 0xa5b1);
        return;
    }
    /* The kernel text base, for naming a slid PC afterwards.  A handoff
     * function runs with the MMU off and no stack, so it cannot say anything
     * itself; it leaves the value where the ordinary code below can print it. */
    adt_wr32((uint8_t *)(scratch + 140), (uint32_t)(at >> 32));
    adt_wr32((uint8_t *)(scratch + 144), (uint32_t)at);
    kc_relax_variant_check(
        (uint8_t *)at,
        len,
        (uint8_t *)(scratch + 88),
        (uint8_t *)(scratch + 128),
        (uint8_t *)(scratch + KC_COUNTS_KERNEL)
    );
    kc_force_root_hash_disabled(
        (uint8_t *)at,
        len,
        (uint8_t *)(scratch + 52)
    );
    kc_skip_rootvp_panic((uint8_t *)at, len, (uint8_t *)(scratch + 148));
    kc_force_root_live_fs((uint8_t *)at, len, (uint8_t *)(scratch + 48));
    kc_force_img4_firmware_ok((uint8_t *)at, len, (uint8_t *)(scratch + 120));
    /*
     * And the privileged layer's own text, which is a separate segment.
     *
     * 17.0 publishes one range, BootKC-rx, that covers everything executable.
     * 15.0.1 publishes the Mach-O's segments one at a time, and PPL is its
     * own -- so a scan of Kernel-__TEXT_EXEC alone misses every site inside
     * it.  That is not a corner: gxf_bootstrap_handler, which is the only
     * place the guarded per-CPU register is written, lives there, and
     * patching the eight readers without the one writer leaves them reading a
     * register nothing has filled.
     */
    {
        uint64_t ppl_at, ppl_len;

        if (map_range(root, "Kernel-__PPLTEXT", &ppl_at, &ppl_len) != 0) {
            kc_relax_variant_check(
                (uint8_t *)ppl_at,
                ppl_len,
                (uint8_t *)(scratch + 100),
                (uint8_t *)(scratch + 132),
                (uint8_t *)(scratch + KC_COUNTS_PPL)
            );
        }
    }
    /*
     * Anything the host worked out about this kernelcache goes on top of the
     * scan above.  The scan matches encodings and so needs no offsets at all,
     * which is why it survives a rebuild; an uploaded table is for the sites
     * that have no encoding to match on.
     */
    /*
     * And the monitor's own copy of libimg4.
     *
     * TXM links the same `img4_firmware_evaluate`, and on 17.0 it is the only
     * img4 evaluation left that this tree does not otherwise reach: the
     * cryptex trust cache is validated **inside TXM**, against an apticket
     * that is genuinely Apple-signed but issued for vresearch101
     * (`CHIP 0xfe01 / BORD 0x90 / SDOM 1`) rather than for d16, so it cannot
     * be re-signed and the chip environment can never match.  Refused, TXM
     * answers status 3, the kernel returns `KERN_FAILURE`, AMFI turns that
     * into `ECANCELED` for dyld's ignition, and userspace never gets
     * `/usr/lib`.
     *
     * Patching the evaluation is enough on its own: `img4_firmware_execute`
     * only skips unwrapping the payload when the evaluation fails, so with it
     * forced to success the callback is handed the real payload range and the
     * trust cache is parsed and loaded for real -- cdhashes and all -- rather
     * than merely reported as loaded.
     *
     * `TXM-rx` is the range SPTM itself looks up in `chosen/memory-map`, and
     * this runs before SPTM has started, while the image is still plain
     * memory.
     */
    {
        uint64_t txm_at, txm_len;

        if (map_range(root, "TXM-rx", &txm_at, &txm_len) != 0) {
            kc_force_img4_firmware_ok(
                (uint8_t *)txm_at,
                txm_len,
                (uint8_t *)(scratch + 168)
            );
        } else {
            adt_wr32((uint8_t *)(scratch + 168), 0xa5c0);   /* no TXM-rx */
        }
    }
    /*
     * Read out the static trust cache region iBoot built, so the shape of it
     * is measured rather than assumed: its address, its length, how many
     * modules it holds and where the first one starts.  A region is
     * `{u32 num_caches; u32 offsets[num_caches]}` followed by the raw
     * modules, and TXM loads every one of them with no img4 at all.
     */
    {
        uint64_t tc_at, tc_len;

        if (map_range(root, "TrustCache", &tc_at, &tc_len) != 0) {
            adt_wr32((uint8_t *)(scratch + 176), (uint32_t)(tc_at >> 32));
            adt_wr32((uint8_t *)(scratch + 180), (uint32_t)tc_at);
            adt_wr32((uint8_t *)(scratch + 184), (uint32_t)tc_len);
            adt_wr32((uint8_t *)(scratch + 188), adt_rd32((uint8_t *)tc_at));
            adt_wr32(
                (uint8_t *)(scratch + 192),
                adt_rd32((uint8_t *)(tc_at + 4))
            );
            /* the first module's own header: version, then its entry count
             * eight bytes past the uuid */
            {
                uint64_t m = tc_at + adt_rd32((uint8_t *)(tc_at + 4));

                adt_wr32((uint8_t *)(scratch + 196), adt_rd32((uint8_t *)m));
                adt_wr32(
                    (uint8_t *)(scratch + 200),
                    adt_rd32((uint8_t *)(m + 20))
                );
            }
        } else {
            adt_wr32((uint8_t *)(scratch + 176), 0xdead);
        }
    }
    patch_store_apply(PATCH_STAGE_KERNEL, (uint8_t *)at);
    {
        uint64_t txm_at, txm_len;

        if (map_range(root, "TXM-rx", &txm_at, &txm_len) != 0) {
            patch_store_apply_txm(PATCH_STAGE_KERNEL, (uint8_t *)txm_at);
        }
    }
    adt_wr32(mark, 0xa5b2);
}

#define KC_NOP          0xd503201fu
#define KC_MOV_XZR      0xaa1f03e0u      /* mov Xt, xzr, Rt in [4:0]  */
#define KC_SSBS_MRS     0xd53b42c0u      /* mrs Xt, S3_3_C4_C2_6      */
#define KC_SSBS_MSR     0xd51b42c0u      /* msr S3_3_C4_C2_6, Xt      */
#define KC_ACNTVCT_MRS  0xd53cfac0u      /* mrs Xt, S3_4_C15_C10_6    */
#define KC_CNTVCT_MRS   0xd53be040u      /* mrs Xt, CNTVCT_EL0        */
#define KC_RT_MASK      0xffffffe0u

/*
 * What has to change in the kernel, and how to find it.
 *
 * Each entry is three instructions that appear together nowhere else, and the
 * index of the one to replace.  Searching for the shape rather than an offset
 * means a different build moves the code without this having to be told, and
 * a shape that is not there is simply skipped.
 *
 * The variant check: arm_init reads what SPTM published and panics unless it
 * matches its own -- release wants zero and the monitor we run is the
 * development build.  Taking out the branch lets an otherwise healthy kernel
 * run beside it.
 *
 * The PSTATE write: the thread switch writes S3_3_C4_C2_6 from a per-thread
 * byte.  CRn 4 is where the PSTATE fields live and this host does not have
 * that one -- the firmware is built for an A15 and the host is an M4 -- so the
 * instruction is UNDEFINED.  That is not a trap the hypervisor can answer: an
 * undefined instruction at EL1 goes to the guest's own vector, and
 * handle_uncategorized turns it into a panic without anyone outside seeing it.
 * The only place to fix it is here, before the kernel runs.
 */
static const struct {
    uint32_t word[3];
    unsigned replace;
    uint32_t with;
} kc_patches[] = {
    { { 0xb949c509u, 0x35000169u, 0xd0ffd008u }, 1, KC_NOP },
                                                /* arm_init variant          */
    /*
     * ApplePMGR's budget for a CPU-complex performance-state change.
     *
     *     panic(cpu 0 ...): "_updateCPUComplexPerfState takes too long
     *                        execTime 45392 us deltaSetPerfState 1087603
     *                        perfDomainID 2" @ApplePMGR.cpp:12995
     *
     * ApplePMGR::start settles the budget as
     *
     *     ldrb w8, [x19, #0xbd0]     ; an override, which nothing here sets
     *     cbz  w8, .default
     *     ldr  w8, [x19, #0xbd4]
     *     b    .store
     * .default:
     *     movz w8, #0xfc80           ; 720000 ticks, 30 ms at 24 MHz
     *     movk w8, #0xa, lsl #16
     * .store:
     *     str  x8, [x19, #0x1d8]
     *
     * so the constant is in the code and nowhere else -- there is no property
     * and no boot-arg that reaches it, and ml-timeout-global-scale does not
     * either: that covers XNU's own MACHINE_TIMEOUT family, not a kext's own
     * arithmetic.  Only the MOVK is rewritten, which lifts the budget to
     * 0x02bffc80 -- 46,136,960 ticks, 1.92 s -- the same sixty-fourfold the
     * boot-args already ask of every timeout the kernel owns.  The assertion
     * still means something; it just measures a machine where an access to
     * pmgr is an exit rather than a bus cycle.
     *
     * The check is only armed at all because the debugger is allowed:
     * ApplePMGR::start reads PE_i_can_has_debugger first and leaves the flag
     * at this+0x13d clear when it is not.  That answer comes from
     * /chosen/debug-enabled and from nothing else (kc:0xFFFFFFF00A9F2138 is
     * the only reader of the property, and the only writer of debug_enabled),
     * which reads one on this board.  It is **not** the production status --
     * an earlier version of this comment tied the two together, and
     * sptm_handoff_production_status now sets that field to one for a reason
     * of its own without disarming this.
     */
    { { 0x529f9008u, 0x72a00148u, 0xf94017f4u }, 1, 0x72a057e8u },
                                                /* pmgr perf-state budget    */
    /*
     * A probe, not a fix: make pmap.c:9080 say which page it refused.
     *
     * `pmap_is_monitor` panics when the page it is handed is not inside
     * [vm_first_phys, vm_last_phys) -- measured on this machine as
     * 0x8_01e7_0000 .. 0x8_ec6b_4000, which is gPhysBase plus gPhysSize and
     * the same range the tree's carveout region-id-24 describes.  The message
     * carries the file and the line and nothing else, and the page number is
     * in a caller-saved register that nothing on the stack keeps, so a
     * post-mortem cannot reach it.
     *
     * What it can reach is x9: `lsl x9, x8, #0xe` computes the physical
     * address at the top of the routine and nothing on the failing path
     * touches it again.  Writing it over the line number spends the line --
     * which is already known, and printed by every other assertion in the
     * file -- to get the address.  The panic then reads
     *
     *     pmap.c:<low 32 bits of the physical address, as a signed decimal>
     *
     * The three words matched are the two that build the expression string
     * and the line itself, which occur together exactly once in 19A348.
     */
    { { 0xb0ffa4e2u, 0x91299842u, 0x52846f01u }, 2, 0xaa0903e1u },
                                                /* pmap.c:9080 -> the page   */
    /*
     * The guarded per-CPU register, moved to the bank this machine can keep.
     *
     * Every place the kernel reaches its own guarded per-CPU block chooses
     * between two registers by the exception level it is at:
     *
     *     mrs xN, currentel
     *     cmp xN, #8
     *     b.eq .gl2
     *     mrs xN, TPIDR_GL1        ; s3_6_c15_c10_1
     *     b   .done
     * .gl2:
     *     mrs xN, TPIDR_GL2        ; s3_6_c15_c11_1
     *
     * A phone runs iOS 15 at EL1 and takes the first arm.  Here the guest is
     * a guest hypervisor, CurrentEL reads 8, and it takes the second -- and
     * TPIDR_GL2 is the one register of the guarded set that nothing carries.
     * Measured in the private context at a CTRR trap: GXF_CONFIG, GXF_ENTRY,
     * GXF_ABORT, SP_GL1, ASPSR_GL1, VBAR_GL1, ESR_GL1, ELR_GL1 and SPSR_GL1
     * all present and right, TPIDR_GL1 zero because nothing writes it, and no
     * slot anywhere holding the block address.  So the value the kernel puts
     * in TPIDR_GL2 survives only until the vCPU is next descheduled, after
     * which ml_get_ppl_cpu_data returns zero inside PPL and
     * pmap_enter_options_internal bzeroes from a null block -- the same
     * instruction on three runs out of three.
     *
     * Taking out the branch sends every one of them down the first arm, which
     * both writes and reads TPIDR_GL1, the slot the kernel does carry.  The
     * two registers hold the same thing; only the bank differs.
     *
     * Four shapes cover nine sites -- one register each for x0, x5 and x12 on
     * the eight readers, and x2 on the one writer in gxf_bootstrap_handler,
     * whose branch also skips a HCR_EL2.TIDCP toggle that on this machine
     * reaches a shadow rather than the register.  Each triple was counted
     * across the whole kernelcache and matches nowhere else.
     */
    { { 0xd5384240u, 0xf100201fu, 0x54000060u }, 2, KC_NOP },
                                                /* GL1 not GL2, x0, x5      */
    { { 0xd5384245u, 0xf10020bfu, 0x54000060u }, 2, KC_NOP },
    { { 0xd538424cu, 0xf100219fu, 0x54000060u }, 2, KC_NOP },
                                                /* and x12, and the writer  */
    { { 0xd5384242u, 0xf100205fu, 0x54000080u }, 2, KC_NOP },
    /*
     * And the one reader that need not consult a banked register at all.
     *
     * ml_get_ppl_cpu_data answers with TPIDR_GLn when the core is guarded and
     * otherwise derives the same address from MPIDR_EL1 -- base plus the core
     * index times 0x180, a computation that cannot come out zero.  Both arms
     * name the same block, so sending it down the second one always removes a
     * banked register from the answer without changing it.  Every null block
     * seen inside PPL so far came back through this function.
     */
    { { 0xd53ef800u, 0xb4000100u, 0xd5384240u }, 1, 0x14000008u },
                                                /* always the MPIDR arm     */
    /*
     * Keep interrupts masked for the length of a call into the layer.
     *
     * ppl_dispatch runs guarded and deliberately restores the caller DAIF
     * around the call it makes on the kernel behalf:
     *
     *     stp x12, x13, [sp, #-0x10]!
     *     msr daif, x20
     *     blraa x10, x9
     *     msr daifset, #7
     *
     * On a phone that is free: an interrupt taken there goes to VBAR_GL and
     * one of the fleh_*_from_ppl handlers, which the kernel has. Here it
     * cannot: the guarded vector is unreachable from outside, so the level
     * lands in VBAR_EL1 instead and the layer is re-entered through the wrong
     * table with the wrong bank behind it. The gate this side keeps holds a
     * level while the guest is guarded, but it can only decide at an exit,
     * and a level raised while the guest was still in the kernel stands
     * across the genter that follows -- which is where it is taken.
     *
     * Masking for the duration closes that. A call into the layer is short
     * and bounded, and the interrupt is a level: it is delivered on the way
     * out, late rather than lost.
     */
    { { 0xa9bf37ecu, 0xd51b4234u, 0xd73f0949u }, 1, KC_NOP },
                                                /* no DAIF inside PPL       */
    /*
     * The same clock, one function further in.
     *
     *     panic(cpu 1 ...): "_waitAPSCPending takes too long exeution time
     *                        8783 us domain 2 apscManSlpStartTsTmp 0,
     *                        CurrentTime 2387645384 counter 0 deltaDPE 0"
     *                        @AppleT8110PMGR.cpp:1901
     *
     * Nothing is actually pending.  _waitAPSCPending polls bit 31 of the APSC
     * status word -- pmgr+0xe20020, one register per cluster at 0x210e20020
     * and 0x211e20020 -- and over a whole boot that bit is never set: the two
     * spin loops turn zero times.  What panics is the check *after* them,
     * which is unconditional and measures the whole call:
     *
     *     mov  w8, #0x4b7c8
     *     ldr  x8, [x23, x8]       ; the budget
     *     cmp  x26, x8             ; now - entry
     *     b.cs .panic
     *
     * On this machine those few reads are VM exits, and CLPC makes about
     * ninety-three thousand of these calls in one boot -- counted from the log,
     * 93851 reads of +0xe20038 at one per call.  Ninety-three thousand rolls
     * of a die that only has to come up once, which is why it was intermittent
     * rather than absent: the same image passed it on the previous boot.
     *
     * Only the branch is rewritten.  Falling through is the path the function
     * already takes when it is inside its budget -- it returns the register it
     * just read and nothing else changes -- so this removes the deadline and
     * not the work.  Raising the budget instead would need the store in
     * ::start, and unlike the perf-state one above that value is not a literal
     * in the code.
     */
    { { 0xf8686ae8u, 0xeb08035fu, 0x54000102u }, 2, KC_NOP },
                                                /* pmgr APSC wait deadline   */
    /*
     * The *other* deadline in the same function, and the one that has been
     * ending every 19A348 UI boot a couple of minutes after the compositor
     * draws the Apple logo.  Read off the frozen machine
     * on an installed-chain boot: the serial log stops mid-stream, four
     * cores sit in `_DebuggerXCall`, cpu4 is inside `_DebuggerXCallEnter`
     * polling `mach_absolute_time`, and cpu5 spins in
     * `_hw_lock_lock_contended` on a lock in `_pmap_cpu_data_array` with DAIF
     * masked -- so it never answers the IPI and the panic never prints.  Its
     * frame chain is machine_idle -> cpu_idle -> ApplePMGR::enterCPUIdle ->
     * _cpuIdle -> cpuComplexIdleEnter -> _waitAPSCPending + 788 -> panic, and
     * the block it lands in carries `MOV W10, #0x772`:
     *
     *     "_waitAPSCPending takes too long exeution time %llu us domain %u
     *      apscManSlpStartTsTmp %llu, CurrentTime %llu counter %llu
     *      deltaDPE %llu"  @AppleT8110PMGR.cpp:1906,  domain 5, counter 15426
     *
     * The entry above guards the +0xe20160 poll and reads its budget out of
     * `[x23, #0x4b7c8]`; this one guards the +0xe20588 poll and its budget is
     * a literal:
     *
     *     lsr  x8, x27, #9        ; elapsed ticks / 512
     *     cmp  x8, #0x177         ; 375 -- 8 ms at 24 MHz
     *     b.cs .panic
     *
     * Widen the shift rather than the compare, because a 12-bit immediate has
     * no room for 375x64: #9 becomes #15, which is the same instruction with
     * immr 15 instead of 9, and the budget becomes 512 ms.  The compare and
     * the branch are untouched, so the check still exists -- it is sixty-four
     * times looser, which is what a vCPU thread that can be descheduled for a
     * whole scheduler quantum needs.  Exactly one site in the kernelcache
     * matches these three words.
     */
    { { 0xd349ff68u, 0xf105dd1fu, 0x54000482u }, 0, 0xd34fff68u },
                                                /* pmgr APSC idle deadline   */
    /*
     * ApplePMGR's budget for one device power-state change.
     *
     *     panic(cpu 1 ...): "_syncDeviceStatusChange takes too long
     *                        execTime 731962 us threshold 30000 us
     *                        PsLevelChangeTime 731957 us turningOn 1
     *                        threadSlept 0" @ApplePMGR.cpp:10008
     *
     * The same arithmetic as the perf-state budget above, in a different
     * method: `_syncDeviceStatusChange` reads mach_absolute_time twice
     * around the change and panics unless the difference is under
     *
     *     cmp   w21, #0                  ; the thread-slept flag
     *     movz  w9, #0xfc80              ; 720000 ticks, 30 ms at 24 MHz
     *     movk  w9, #0xa, lsl #16
     *     movz  w10, #0x9700             ; 21600000 ticks, 900 ms, if it slept
     *     movk  w10, #0x149, lsl #16
     *     csel  x9, x10, x9, ne
     *     cmp   x8, x9
     *     b.cs  .panic
     *
     * so only the arm that was taken is lifted: `turningOn 1 threadSlept 0`
     * is the 30 ms one.  The MOVK becomes #0x2bf, making the budget
     * 0x02bffc80 -- 46,136,960 ticks, 1.92 s -- the same sixty-fourfold as
     * the perf-state entry, and 2.6x the 731 ms that was measured.  The
     * 900 ms arm is left alone: it already stands thirty times over what
     * this machine took.
     *
     * What it is really measuring here is the DCP.  With no display power
     * assertion held, AppleDCPExpert powers the coprocessor up for one link
     * RPC and straight back down, and that power-on runs through
     * `AppleT8110MemCacheController` inside ApplePMGR's own command gate
     * (see [[dcp-powerdown-wedges-on-mcc]]).  Lifting the budget keeps the
     * machine alive to be measured; it does not make the cycling right.
     *
     * The three words occur exactly once in the 19A348 DEVELOPMENT
     * kernelcache this boots (counted: one hit, at file offset 0x21a8ac4).
     */
    { { 0x529f9009u, 0x72a00149u, 0x5292e00au }, 1, 0x72a057e9u },
                                                /* pmgr device-status budget */
    /*
     * ApplePMGR's budget for one CPU perf-state request -- and the panic that
     * cannot print itself.
     *
     *     panic(cpu 1 ...): "ApplePMGR: %s:%u " "Lock took %lld us ..."
     *                       @ApplePMGR.cpp:10755
     *
     * The third of the same family.  `_handleCPUPerfStateRequest` masks
     * interrupts, takes the perf-state spin lock, calls
     * `_updateCPUComplexPerfState` and times the whole section:
     *
     *     movz w8, #0x7e40           ; 360000 ticks, 15 ms at 24 MHz
     *     movk w8, #0x5, lsl #16
     *     cmp  x0, x8
     *     b.cc .work
     *     bl   .cold                 ; panic
     *
     * Here the constant is a literal in the method -- unlike the :12995 one
     * above, which ::start stores -- so only the MOVK is rewritten, lifting
     * the budget to 0x02bf7e40, 46,104,128 ticks, 1.92 s: the same
     * sixty-fourfold, and the same number, as the two entries above.  Falling
     * through is the path the method already takes inside its budget, so this
     * removes the deadline and not the work.
     *
     * Found 2026-09-29 on a 19A348 restore boot, which
     * stopped printing with `ASR RESTORE PROGRESS: 90%` behind it,
     * both halves of the boot still alive and `VM status: running`.  It had
     * already panicked, and could not say so.  Read off the frozen machine
     * nine hours later, PCs unmoved across five samples:
     *
     *   cpu1  mach_absolute_time <- DebuggerXCallEnter+0x3b0
     *         <- handle_debugger_trap <- kdp_trap <- sleh_synchronous
     *         <- fleh_synchronous <- panic_trap_to_debugger <- panic
     *         <- _handleCPUPerfStateRequest.cold.1
     *         <- ApplePMGR::_handleCPUPerfStateRequest+0xf4
     *         <- clpc::cpu::DVFManager::requestPerformanceChange
     *         <- clpc::CLPC::performanceChangeHandler <- IOWorkLoop
     *   cpu4  hw_lock_lock_contended <- ApplePMGR::_cpuIdle
     *         <- ApplePMGR::exitCPUIdle <- cpu_idle_exit <- cpu_idle
     *         <- machine_idle <- processor_idle <- idle_thread
     *   cpu0, cpu2, cpu3, cpu5   DebuggerXCall+0x148
     *
     * `debugger_cpu` was 1, `debugger_message` and `debugger_panic_str` both
     * NULL, and `debug_buf_ptr == debug_buf_base` -- not one byte of the
     * paniclog written.  The panic stops all the way inside
     * `DebuggerXCallEnter`, waiting for a core that cannot answer: cpu4 is in
     * the PMGR lock with DAIF masked (PSTATE 0x...3c4), so it never takes the
     * debugger IPI, the cross-call never completes and nothing reaches the
     * UART.  **A boot that dies here is indistinguishable from an idle hang**
     * -- machine running, every PC frozen, both logs quiet -- so the first
     * question on a silent boot is not `info status` but where the PCs are.
     *
     * Why it is intermittent: the budget is wall clock, and every pmgr access
     * inside the section is a VM exit, so the section competes with whatever
     * else the host is doing -- here ASR migrating a volume.  And it is armed
     * at all only because /chosen sends up a development part, the same trade
     * the :12995 entry above names.
     *
     * The three words occur exactly once in the 19A348 DEVELOPMENT
     * kernelcache this boots (counted: one hit, at file offset 0x21aaef0).
     * The movz/movk/cmp triplet on its own occurs twenty-four times -- the
     * same 15 ms compare inlined all over the kext -- which is why the branch
     * is in the pattern and the MOVK, not the branch, is the word replaced.
     */
    { { 0x72a000a8u, 0xeb08001fu, 0x540001c3u }, 0, 0x72a057e8u },
                                                /* pmgr perf-state lock      */
    /*
     * The same 30 ms budget as the `:12995` row above, at the other site that
     * reads it: the tail of _updateCPUComplexPerfState itself.
     *
     * A STOPGAP.  The kernel's timeouts are this machine's performance gauge,
     * and this one is not wrong: a perf-state change took 96-240 ms where the
     * part allows 30, most likely because the vCPU writing PMGR waits for the
     * emulator's global lock behind other work.  Making those exits lockless
     * in the emulator is the fix; once it lands this row comes out and the
     * stock budget has to hold.  It is here so 19A348 can be walked to its
     * activation page in the meantime.
     *
     *     panic: "ApplePMGR: _updateCPUComplexPerfState:11532 takes too long
     *             execTime 240314 us deltaSetPerfState 235 perfDomainID 5"
     *
     * read off the frozen machine twice (the panicking core's frame chain,
     * the arguments off its stack) -- once in a DFU restore during the volume
     * copy, once on the installed chain at Setup's Quick Start (execTime
     * 96483 us).  Both times the same freeze as the lock row above describes:
     * cpu1 in DebuggerXCallEnter from clpc::DVFManager ->
     * _handleCPUPerfStateRequest, cpu5 spinning on the PMGR lock in _cpuIdle
     * with DAIF masked, nothing on the UART, the emulator at ~590 % CPU.
     *
     * The compare is
     *
     *     movz w8, #0xfc80            ; 720000 ticks, 30 ms at 24 MHz
     *     movk w8, #0xa, lsl #16
     *     cmp  x0, x8                 ; x0 = now - entry
     *
     * which 20A362's kernel stage patch row (pmgr_perfstate_timeout) found by
     * hand: "`bl mach_absolute_time / sub / mov w8,#0xafc80 / cmp x0,x8 /
     * b.cs` ... the ROM's own row does not match it -- its third word is a
     * ldr here".  So the third word here
     * is the `cmp`, and only the MOVK is rewritten, to the same 1.92 s as the
     * other three.
     *
     * Counted, not assumed -- this function run over the image
     * (tests/runtime/boot/sptm_handoff_kc_patches_tests.c, KC_IMAGE=), and on
     * the machine the per-row counts at scratch + 0x400:
     * one hit in 19A348's development kernelcache, in
     * ApplePMGR::_updateCPUComplexPerfState+0x21c, and the movz/movk pair
     * occurs nowhere else in __TEXT_EXEC; one hit in 20A362's release
     * kernelcache, at 0xfffffff0090fc9c8 -- the compare in front of the b.cs
     * its kernel stage row NOPs, so the two touch different words; none in
     * the 21A329, 22G100, 23A341 or 24A437 release kernelcaches, whose
     * movz/movk pairs go on to a `ldr` or a `mov`.
     */
    { { 0x529f9008u, 0x72a00148u, 0xeb08001fu }, 1, 0x72a057e8u },
                                                /* pmgr perf-state tail      */
    /*
     * The same 15 ms lock budget again, in the core that *waits* for it.
     *
     *     panic(cpu 4 ...): "ApplePMGR: %s:%u " "Lock took %lld us ..."
     *                       <- _cpuIdleTimer.cold.1
     *
     * `_cpuIdleTimer` does not time a section -- it times the *acquire* of the
     * very lock the entry above holds, against the same literal and recording
     * into the same slot at `this+0x2f998`:
     *
     *     bl   _mach_absolute_time   ; x24 = t0
     *     ldr  x0, [x19]             ; the CPU-complex lock
     *     bl   _lck_spin_lock        ; <- the wait being measured
     *     bl   _mach_absolute_time
     *     sub  x0, x0, x24
     *     movz w8, #0x7e40           ; 360000 ticks, 15 ms at 24 MHz
     *     movk w8, #0x5, lsl #16
     *     cmp  x0, x8
     *     b.cs .cold                 ; panic
     *
     * So raising only the holder's copy above was half a fix: a slow holder no
     * longer panics itself, but any core entering or leaving idle behind it
     * still does.  Same MOVK, same 0x02bf7e40, same 1.92 s.
     *
     * Note the branch is the other way round from the entry above -- `b.cs`
     * into the panic rather than `b.cc` over it -- which is the only word that
     * differs, and the one that makes each pattern name its own site.
     *
     * Seen twice.  First as `Lock took 29863 us`, once in 685 installed
     * boots and not in the five after it, so it was left alone.
     * Then it ended an installed UI boot after about a
     * minute, and a second time is no longer a die roll worth paying for --
     * every hook attempt in that boot read as "the thing I just changed wedged
     * the guest" when it was this.  Read off the frozen machine:
     *
     *   cpu4  _mach_absolute_time+0x2c, LR _DebuggerXCallEnter+0x3b0
     *         <- _handle_debugger_trap <- _kdp_trap <- _sleh_synchronous
     *         <- _fleh_synchronous <- _panic_trap_to_debugger <- _Assert
     *         <- ApplePMGR::_cpuIdleTimer(CPUCore *, ulong long *).cold.1
     *         <- ApplePMGR::_cpuIdle+0x98 <- ApplePMGR::enterCPUIdle+0x84
     *         <- _cpu_idle+0x23c <- _machine_idle+0xf8
     *   cpu5  _hw_lock_lock_contended+0x118, on a lock inside
     *         _pmap_cpu_data_array -- DAIF masked, so it never answers the
     *         cross-call and nothing reaches the UART
     *   cpu0-3  _DebuggerXCall+0x148        cpu6  SEP
     *
     * The three words occur exactly once in the 19A348 DEVELOPMENT kernelcache
     * this boots (one occurrence in the image: file offset 0x21aba88, function
     * 0xfffffff0091afa18).  The movz/movk/cmp triplet alone occurs twenty-four
     * times, same as above.
     */
    { { 0x72a000a8u, 0xeb08001fu, 0x54000582u }, 0, 0x72a7ffe8u },
                                                /* pmgr idle-timer lock      */
    /*
     * The same 15 ms acquire check, inlined -- twenty of it.
     *
     * Raising the row above did not stop the assert, and the frozen machine
     * said why: the cold block returns into `ApplePMGR::_cpuIdle+0x98`, not
     * into `_cpuIdleTimer`.  `_cpuIdleTimer` is inlined into `_cpuIdle`, the
     * inlined copy carries its own compare, and the frame's
     * `_cpuIdleTimer.cold.1` is only the nearest preceding symbol -- the call
     * is to `_cpuIdle.cold.2`.  Reading the symbol as the caller is what sent
     * two boots after the wrong instruction.
     *
     * The branch runs the other way here (`b.cc` over the call, where the
     * standalone copy has `b.cs` into it), which is the only word that differs
     * and the reason this needs a row of its own.
     *
     * **This signature matches twenty sites, and all twenty are meant.**  The
     * scan tries every address and breaks only out of the row loop, so one row
     * patches them all; they are the same check against the same literal, and
     * raising a budget can only remove a panic, never add behaviour.
     */
    { { 0x72a000a8u, 0xeb08001fu, 0x54000103u }, 0, 0x72a7ffe8u },
                                                /* pmgr inlined idle lock    */
    /*
     * Raised a second time, from 1.92 s to 44.7 s (MOVK #0x2BF -> #0x3FFF, so
     * the budget is 0x3FFF7E40 ticks at 24 MHz).
     *
     * 1.92 s was enough for every headless boot -- those ran for hours -- and
     * is not enough with SpringBoard enabled: the boot wedges on this assert
     * about twenty seconds in, before a shell is up and with no capture taken, so
     * the capture is not what stalls it.
     *
     * The frozen machine says why, and it is not the guest's fault.  **No core
     * holds the lock.**  The waiters sit in `_hw_lock_lock_contended`, which
     * spins to acquire; every other core is in `DebuggerXCall` or is the one
     * panicking.  A lock with no holder that still takes seconds to acquire is
     * the host's scheduler: seven vCPU threads on a 6+4 machine, and a
     * descheduled one blows any budget in this class.  The same reasoning
     * raised the two sibling entries above.
     *
     * Still a bounded compare rather than a NOP, so a real lock-up still
     * panics instead of hanging silently.
     */
    /*
     * The display power assertion the boot takes, never given back.
     *
     * `AppleDCPExpert::start` arms `_bootCompleteTimeout` with 60000 ms and
     * that handler releases the boot display assertion
     * ([[dcp-powerdown-wedges-on-mcc]]):
     *
     *     mov  w1, #0            ; assert = false
     *     mov  x2, x0
     *     mov  x17, x9
     *     blraa x8, x17          ; setPowerAssertion(false), vtable +0x548
     *     ...                    ; then the "BootComplete" notification
     *
     * With nothing else holding one the DCP is powered down, and on this
     * machine that is what black-screens it.  Measured over a boot of the
     * installed 19A348: `A407 swap_start` is answered 1919 times, `A500
     * link_shutdown_signal` follows *immediately* -- with not one call in
     * between -- the core is stopped and restarted, and the swap the AP just
     * opened comes back as `A429` carrying that same id, cancelled.  One RPC
     * per power cycle, 3841 cycles, and `A408 swap_submit_dcp` never once.
     *
     * The restore does not do this: there the same 60 s assertion is still
     * held, the order is `A468 setPowerState`, `A407 swap_start`, `A408
     * swap_submit`, and the panel draws -- the Apple logo and the progress
     * bar, 50819 non-black pixels in layer 1 of swap 1.  So the cycling
     * itself is not the defect (the restore cycles too, 198 times); having
     * *no holder* is.
     *
     * Rewriting the flag to 1 keeps the assertion the boot already took.  The
     * BootComplete notification still goes out, so nothing that waits on it
     * is affected; what changes is only that the display stays powered.  It
     * is the same trade as the two budgets above -- an assertion that means
     * something on hardware, measuring a machine where powering the DCP up
     * runs through ApplePMGR's command gate and costs 731 ms.
     *
     * The three words occur exactly once in the 19A348 DEVELOPMENT
     * kernelcache (counted: one hit, at file offset 0x170461c).
     */
    { { 0x52800001u, 0xaa0003e2u, 0xaa0903f1u }, 0, 0x52800021u },
                                                /* keep the boot display on  */
    /*
     * AMX: this SoC has it, the M4 underneath does not, and userland believes
     * the SoC.
     *
     * The M4 replaced AMX with SME -- `sysctl -a` on the host has no `amx`
     * line at all and `hw.optional.arm.FEAT_SME` is 1 -- so an AMX encoding is
     * unallocated, EL1's own vector takes it, and the process dies.  Measured:
     * SpringBoard, SIGILL, `codes 0x1, 0x0020100a`, which is
     * `0x00201000 | (op << 5) | Rn`, an `amx_ldx x10`.  The site is Accelerate
     * using the AMX registers as a wide memcpy inside
     * `vVertical_Scale_ARGB_8888_Accelerate`, reached from
     * `VTPixelTransferSessionTransferImage` -- how the wallpaper is resized
     * once there is no GPU to do it, so the lock screen came up blank about
     * half the time.  Same family as the SSBS scan below, answered the same
     * way.
     *
     * `commpage_populate` on 19A348 detects nothing; for a t8110 all three
     * values are folded constants:
     *
     *     mov  w15, #1
     *     adrp x16, ... ; str w15, [x16, #0xd3c]   ; _gAMX        = 1
     *     mov  w16, #3
     *     adrp x17, ... ; str w16, [x17, #0xd40]   ; _gAMXVersion = 3
     *     adrp x16, ... ; str w15, [x16, #0xd44]   ; _gUCNormalMem, left alone
     *     mov  w15, #0x3c80
     *     movk w15, #0x1900, lsl #16               ; 0x19003c80 >> 27 == 3
     *     orr  w9, w15, w9, lsl #16                ; ... into the commpage
     *
     * **The commpage is the one that matters.**  Zeroing the two globals was
     * measured first and changed nothing: they are only `hw.optional.amx`, and
     * nothing consults it.  What Accelerate consults is
     * `_get_cpu_capabilities()`, and vImage takes the version straight out of
     * it -- `_vImageHorizontalShear_Planar8+0x110`:
     *
     *     bl   j___get_cpu_capabilities
     *     ubfx w11, w0, #0x1b, #0x4        ; bits 30:27
     *     ...
     *     cbz  w11, <the plain kernel>
     *     bl   _vHorizontal_Scale_Planar_UInt8_Accelerate
     *
     * so zero there means "no AMX" and the paired non-Accelerate kernel runs
     * instead.  Rewriting the MOVK to #0x100 leaves 0x01003c80 -- every other
     * advertised bit as it was, the AMX version zero.  The globals are zeroed
     * too, so the sysctl stops disagreeing with the commpage; the stores are
     * rewritten rather than the movs, because w15 also feeds `_gUCNormalMem`.
     *
     * Known gap: the ARGB8888 pair has **no** plain sibling in this build, so
     * a scale that takes that path still has nowhere to go.  Planar8 and
     * CbCr8, which the wallpaper uses, both have one.
     *
     * Each window occurs exactly once in the 19A348 development kernelcache
     * (counted: file offsets 0xeb4e9c, 0xeb4ea8, 0xeb4ebc).
     */
    { { 0x5280002fu, 0xf0ffd1f0u, 0xb90d3e0fu }, 2, 0xb90d3e1fu },
                                                /* _gAMX        = 0          */
    { { 0x52800070u, 0xf0ffd1f1u, 0xb90d4230u }, 2, 0xb90d423fu },
                                                /* _gAMXVersion = 0          */
    { { 0x5287900fu, 0x72a3200fu, 0x2a0941e9u }, 1, 0x72a0200fu },
                                                /* commpage AMX version = 0  */
    /*
     * ApplePMGR's rail-request check, which this machine has nothing to say to.
     *
     * The driver walks its voltage tables and asks every rail for the domain's
     * voltage before it starts the performance domains.  The check that
     * follows compares what it read against a limit and asserts when the
     * request is over it:
     *
     *     panic(cpu 0 caller 0xfffffff04e4eb094): "ApplePMGR: virtual void
     *     ApplePMGR::initDriver(IOService *):2002 " "VENC-AVE-VNOM 110 request
     *     AVE.1 > 1\n" @ApplePMGR.cpp:2002
     *
     * Measured 2026-09-26 on the restore chain, at the first boot that got far
     * enough to run AppleT8110PMGR::initDriver.  There is no rail here to ask
     * and no regulator to answer, so the answer is whatever the emulated PMGR
     * happens to return; the request is not going to be delivered either way.
     * What the check buys on a phone -- a rail driven past its rating -- cannot
     * happen on a machine that has none, and it costs the whole boot.
     *
     * The three words are the two that build the message and the call that
     * formats it, and they occur together once in the 67 MB kernelcache
     * (counted: file offset 0x1f7f088, com.apple.driver.ApplePMGR).  The call
     * is replaced with a NOP, so the assert tail returns to its caller.
     */
    { { 0x91199c00u, 0xf90003eau, 0x940026f7u }, 2, KC_NOP },
                                                /* pmgr rail request         */
    /*
     * The next one along, and the same shape.
     *
     * With the rail-request check out of the way the driver carried on into
     * the performance counters and asserted again:
     *
     *     "device perf counter flag is set with perf counter V2 deviceID
     *      0x7220" @ApplePMGR.cpp:2046
     *
     * The flag is the driver's own, read from the tree the boot chain built;
     * what it disagrees with is the counter version this part reports, which
     * is the machine model's business and not something the guest can settle.
     * Same treatment, same reasoning: the check protects a phone's counters.
     *
     * Counted once in the kernelcache at file offset 0x1f7f0dc.
     */
    { { 0xb0ff3680u, 0x911a7c00u, 0x940026e4u }, 2, KC_NOP },
                                                /* pmgr perf counters        */
    /*
     * And the one after that, which asks the machine a question it cannot
     * answer: whether this part has a die number or a reserved CPU-complex
     * device id.  Neither is modelled, so `die` reads zero and the REQUIRE
     * fails.
     *
     *     "die > 0 || perfDomain->cpuComplex->deviceID == kIDReserved"
     *     @ApplePMGR.cpp:2229
     *
     * Counted once at file offset 0x1f7f10c.
     */
    { { 0xb0ff368au, 0x9124194au, 0x97ff97deu }, 2, KC_NOP },
                                                /* pmgr die / complex id     */
    /*
     * The performance-state group index against the count the driver built.
     *
     *     "psRegGroup < _psRegGroupCount" @ApplePMGR.cpp:2089
     *
     * Filling the tables and the scalars beside them (see
     * sptm_handoff_pmgr_states) does not settle this one: the group index
     * comes from the device table the tree carries, which iBoot also derives,
     * and the mismatch is between two numbers this machine made up.  Nothing
     * downstream reads the group; the register it names is one the model
     * answers for directly.
     *
     * Counted once at file offset 0x1f7f13c.
     */
    { { 0xb0ff368au, 0x911d394au, 0x97ff97d2u }, 2, KC_NOP },
                                                /* pmgr ps reg group         */
    /*
     * And the same for the register index, one line later in the source:
     *
     *     "psRegIndex < _getMaxDevicePsRegSize()" @ApplePMGR.cpp:2090
     *
     * Both numbers come out of the device table the tree carries, so both are
     * this machine's own arithmetic disagreeing with itself.  Same treatment.
     *
     * Counted once at file offset 0x1f7f16c.
     */
    { { 0xb0ff368au, 0x911db14au, 0x97ff97c6u }, 2, KC_NOP },
                                                /* pmgr ps reg index         */
    { { 0x97ff9821u, 0xaa0003feu, 0x97ff97bcu }, 2, KC_NOP },   /* auto: ApplePMGR: virtual void ApplePMGR::initDriver(IOService *):2213 REQUIRE failed: false */
    { { 0xb0ff368au, 0x9123494au, 0x97ff97b0u }, 2, KC_NOP },   /* auto 0xfffffff04e4eb1c8 */
    /* AUTO-ASSERTS */
};

/*
 * What the monitor assumes about where it is, corrected in place.
 *
 * SPTM is built for a machine where it owns EL2.  Here it runs at hardware EL1
 * with FEAT_NV2 in front of it, so the assumption fails in five separate ways,
 * each of them a single instruction:
 *
 *   CNTHCTL_EL2, CNTVOFF_EL2            writes that would land on the controls
 *                                       this side depends on -- sent to a
 *                                       register nothing reads instead.
 *
 * HCR_EL2 was sunk with those two and must not be.  SPTM is the only thing in
 * the whole chain that sets HCR_EL2.TGE, and it does so at four sites
 * (`mrs x10, hcr_el2` / `orr x10, x10, #0x8000000`).  The accelerator services
 * a trapped HCR_EL2 into the guest's own VNCR word, not into the controls it
 * runs the vCPU with, so the write does no harm there -- and the kernel needs
 * its effect: every one of XNU's eight vector slots reads HCR_EL2 and takes
 * `CurrentEL == EL2 && TGE == 0` to mean "this exception belongs to the guest
 * I am running", and under FEAT_NV the first half is true by construction.
 * Sunk, a secondary core -- which this ROM never runs on, so it never inherits
 * the E2H|TGE that start.S leaves on cpu0 -- sends its first exception of any
 * class into the hv guest handler, which dereferences TPIDR_EL2 and panics
 * "Synchronous exception taken while SP1 selected" four seconds into launchd.
 * Measured on 24A437 (twice); with TGE
 * present the chain runs past launchd's ignition.  SPTM also compares HCR_EL2
 * against RW|E2H|APK|API exactly, so the bit has to come from SPTM's own write
 * rather than be arranged before it.
 *   MDSCR_EL1                           a write that is swallowed on the way
 *                                       through; dropped rather than pretended.
 *   ACFG_EL1                            implementation-defined, so NV2 does not
 *                                       carry it and every touch arrives as an
 *                                       undefined instruction.  This early the
 *                                       monitor's vectors are still sixteen
 *                                       "stop here" branches, so it parks on
 *                                       one and the machine sits there.  Apple
 *                                       agrees this is the difference: the
 *                                       build shipped for virtual machines
 *                                       touches ACFG_EL1 nowhere, the one for
 *                                       the part touches it nine times.
 *   CFP/DVP/COSP/CPP RCTX               FEAT_SPECRES, which the part has and
 *                                       this host has not.  t8110 reports
 *                                       ID_AA64ISAR1_EL1.SPECRES == 1; the M4
 *                                       reports 0 (hw.optional.arm.FEAT_SPECRES
 *                                       is 0) and the instruction is undefined
 *                                       here.  24A437's SPTM has exactly one,
 *                                       a `cfp rctx, x8` in the tail of its
 *                                       root-table switch, reached only when
 *                                       the ASID generation rolls over --
 *                                       which is why it costs twenty to forty
 *                                       seconds of userspace to reach.
 *                                       21A329's SPTM, TXM and both
 *                                       kernelcaches contain none, so this
 *                                       class rewrites nothing on any earlier
 *                                       build.  Undefined at EL1 goes to the
 *                                       guest's own vector, and the guest is
 *                                       inside the monitor, so the hardware
 *                                       sends it through VBAR_EL1 -- XNU's
 *                                       table -- rather than VBAR_GL.  That
 *                                       page is not executable at the
 *                                       monitor's guarded level, so the fetch
 *                                       takes a permission fault, which *is*
 *                                       routed to the guarded vectors, and
 *                                       SPTM's unconditional "Current EL, SPx,
 *                                       Synchronous" slot panics "[SPTM
 *                                       Dispatch] Synchronous exception taken
 *                                       while SP1 selected".  Measured on the
 *                                       frozen machine, cpu3 stopped in that
 *                                       handler: ELR_EL1 0xfffffff02c659010
 *                                       names the `cfp`, ESR_EL1 0x02000000 is
 *                                       EC 0 with no ISS, SPSR_EL1 0x20000204
 *                                       is EL1t with I and F clear, and the
 *                                       ELR_GL1/FAR_GL1 the panic prints are
 *                                       the kernel's __TEXT_EXEC base, which
 *                                       is its VBAR_EL1.  A speculation
 *                                       restriction that does nothing is
 *                                       sound: it weakens a Spectre mitigation
 *                                       inside an emulator and nothing else.
 *
 * Matching the encoding rather than the surroundings is what makes it complete
 * -- these sites have no signature worth writing, and one missed is one boot
 * spent finding it.  The sixteenth thing SPTM needs, its exception-state reads,
 * is an expansion rather than a substitution and is not done here yet.
 *
 * Where the image starts is not named by a constant any more.  The entry and
 * the span used to be compiled in (0x64388 and 0x98000, the 21A329 monitor);
 * the 27.0 restore monitor is entered at 0x9c388 and spans 0xfc000, and the
 * next build will move both again.  So the base is found instead: iBoot loads
 * the Mach-O whole, header first, so the __TEXT page holds 0xfeedfacf, and the
 * entry iBoot is about to jump to is `__TEXT vmaddr + (LC_UNIXTHREAD pc -
 * __TEXT vmaddr)` -- a candidate is asked to prove that with its own load
 * commands before anything is rewritten in it.  A magic word that is data in
 * the payload, rather than its header, cannot answer the question.
 */
#define SPTM_MACHO_MAGIC    0xfeedfacfu
#define SPTM_PROBE_LIMIT    0x200000u   /* furthest __TEXT may sit below entry */
#define SPTM_SPAN_CAP       0x200000u   /* never scan past this, header or not */

#define LC_SEGMENT_64       0x19u
#define LC_UNIXTHREAD       0x05u

/* arm_thread_state64_t pc: 16 bytes of command, then x0..x28, fp, lr, sp. */
#define LC_UNIXTHREAD_PC    (16u + 29u * 8u + 3u * 8u)

/* The two segment names, as the two words their padding makes of them. */
#define SEGNAME_TEXT_0      0x45545f5fu /* "__TEXT" */
#define SEGNAME_TEXT_1      0x00005458u
#define SEGNAME_DATA_0      0x41445f5fu /* "__DATA" */
#define SEGNAME_DATA_1      0x00004154u

#define RT_MASK         0xffffffe0u
#define MSR_CNTHCTL_EL2 0xd51ce100u
#define MSR_CNTVOFF_EL2 0xd51ce060u
#define MSR_MDSCR_EL1   0xd5100240u
#define MSR_ACFG_EL1    0xd51cfc00u
#define MRS_ACFG_EL1    0xd53cfc00u
#define MSR_SINK_5      0xd51fffa0u
#define MSR_SINK_4      0xd51fff80u
#define MOVZ_X0_0       0xd2800000u

/* FEAT_SPECRES, `sys #3, c7, c3, #{4,5,6,7}, Xt`; op2 is bits 7:5, Rt is 4:0. */
#define SYS_CFP_RCTX    0xd50b7380u
#define SYS_DVP_RCTX    0xd50b73a0u
#define SYS_COSP_RCTX   0xd50b73c0u
#define SYS_CPP_RCTX    0xd50b73e0u

/*
 * The image under an entry point, or null.
 *
 * Everything is read four bytes at a time: the handoff runs with the MMU off,
 * where the memory is Device and a wider or unaligned load aborts.
 */
HANDOFF static uint8_t *sptm_image_base(uint8_t *entry, uint64_t *span)
{
    uint8_t *first = (uint8_t *)((uint64_t)entry & ~0xfffull);
    uint64_t back;

    for (back = 0; back <= SPTM_PROBE_LIMIT; back += 0x1000u) {
        uint8_t *candidate = first - back;
        uint32_t ncmds;
        uint32_t sizeofcmds;
        uint64_t text_vmaddr = 0;
        uint64_t data_end = 0;
        uint64_t seg_max = 0;
        uint64_t pc = 0;
        int have_text = 0;
        int have_pc = 0;
        uint32_t off;
        uint32_t i;

        if (adt_rd32(candidate) != SPTM_MACHO_MAGIC) {
            continue;
        }
        ncmds = adt_rd32(candidate + 16);
        sizeofcmds = adt_rd32(candidate + 20);
        if (ncmds == 0 || ncmds > 64 ||
            sizeofcmds < ncmds * 8u || sizeofcmds > 0x8000u) {
            continue;
        }
        off = 32;
        for (i = 0; i < ncmds && off + 8u <= 32u + sizeofcmds; i++) {
            uint32_t cmd = adt_rd32(candidate + off);
            uint32_t cmdsize = adt_rd32(candidate + off + 4);

            if (cmdsize < 8 || off + cmdsize > 32u + sizeofcmds) {
                break;
            }
            if (cmd == LC_SEGMENT_64 && cmdsize >= 72) {
                uint64_t vmaddr = adt_rd32(candidate + off + 24) |
                                  ((uint64_t)adt_rd32(candidate + off + 28) << 32);
                uint64_t vmsize = adt_rd32(candidate + off + 32) |
                                  ((uint64_t)adt_rd32(candidate + off + 36) << 32);

                if (adt_rd32(candidate + off + 8) == SEGNAME_TEXT_0 &&
                    adt_rd32(candidate + off + 12) == SEGNAME_TEXT_1) {
                    text_vmaddr = vmaddr;
                    have_text = 1;
                }
                if (adt_rd32(candidate + off + 8) == SEGNAME_DATA_0 &&
                    adt_rd32(candidate + off + 12) == SEGNAME_DATA_1) {
                    data_end = vmaddr + vmsize;
                }
                if (vmaddr + vmsize > seg_max) {
                    seg_max = vmaddr + vmsize;
                }
            } else if (cmd == LC_UNIXTHREAD && cmdsize >= LC_UNIXTHREAD_PC + 8u) {
                pc = adt_rd32(candidate + off + LC_UNIXTHREAD_PC) |
                     ((uint64_t)adt_rd32(candidate + off + LC_UNIXTHREAD_PC + 4) << 32);
                have_pc = 1;
            }
            off += cmdsize;
        }
        if (!have_text || !have_pc || pc < text_vmaddr) {
            continue;
        }
        if ((uint64_t)entry - (uint64_t)candidate != pc - text_vmaddr) {
            continue;   /* a magic word in the payload, not the header */
        }
        if (data_end != 0 && data_end > text_vmaddr) {
            *span = data_end - text_vmaddr;
        } else if (seg_max > text_vmaddr) {
            *span = seg_max - text_vmaddr;
        } else {
            continue;
        }
        if (*span > SPTM_SPAN_CAP) {
            *span = SPTM_SPAN_CAP;
        }
        return candidate;
    }
    return 0;
}

HANDOFF static void sptm_apply_patches(uint8_t *entry, uint8_t *mark)
{
    uint64_t span = 0;
    uint8_t *base = sptm_image_base(entry, &span);
    unsigned n = 0;
    uint64_t at;

    if (base == 0) {
        adt_wr32(mark, 0x5b7fu);                /* the header did not answer */
        return;
    }
    say("OpenBootROM: sptm image ");
    say_hex((uint64_t)base);
    say(" span ");
    say_hex(span);
    say("\n");

    for (at = 0; at + 4 <= span; at += 4) {
        uint32_t w = adt_rd32(base + at);
        uint32_t rt = w & 0x1fu;
        uint32_t to;

        /*
         * The one GXF instruction the part has and this host does not.
         *
         * `gexit` is 0x00201400 and `genter` 0x00201420; this is 0x00201462,
         * which sets a bit above the five the immediate occupies -- XNU's own
         * ISS_GENTER_IMM_MASK is 0x1f.  Undefined here, and undefined at EL1
         * goes to the guest's own vector, so nothing outside can answer it:
         * SPTM decodes its own faulting instruction, decides the immediate is
         * not one it knows, and takes nested_panic_bad_genter_immediate.
         *
         * Exactly one site, at the head of CleanPoC_DcacheRegion_Force_nopreempt
         * and followed by an `isb` and a clear of bit 11 in S3_0_C15_C4_*.
         * Whether the guarded entry is load-bearing there is the question this
         * is asking: if it is, the body fails some other way and that says so.
         */
        if (w == 0x00201462u) {
            adt_wr32(base + at, KC_NOP);
            adt_clean(base + at, base + at + 4);
            n++;
            continue;
        }
        switch (w & RT_MASK) {
            case MSR_CNTHCTL_EL2: to = MSR_SINK_5 | rt; break;
            case MSR_CNTVOFF_EL2: to = MSR_SINK_4 | rt; break;
            case MSR_MDSCR_EL1:   to = KC_NOP;          break;
            case MSR_ACFG_EL1:    to = KC_NOP;          break;
            case MRS_ACFG_EL1:    to = MOVZ_X0_0 | rt;  break;
            case SYS_CFP_RCTX:
            case SYS_DVP_RCTX:
            case SYS_COSP_RCTX:
            case SYS_CPP_RCTX:    to = KC_NOP;          break;
            default:              continue;
        }
        adt_wr32(base + at, to);
        adt_clean(base + at, base + at + 4);
        n++;
    }
    __asm__ volatile("ic ialluis" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    adt_wr32(mark, 0x5b70u | (n << 16));
}

/*
 * iBEC, at the handoff iBSS makes to it.
 *
 * iBSS and iBEC are the same binary here, so the table that applies to one
 * applies to the other -- including the two words that divert the trampoline
 * to this ROM, which is why both handoffs now arrive at the same entry and
 * why the scratch page has to say which one this is.  Nothing patches a stage
 * that arrives through iBoot's own loader, so before this the same table was
 * applied to the image on disk by hand.
 *
 * X30 is where iBoot is about to jump, and the image is entered at its own
 * first word, so that is the base the table's offsets are counted from.
 */
void ibec_handoff_patch(uint64_t entry);
HANDOFF void ibec_handoff_patch(uint64_t entry)
{
    say("OpenBootROM: handoff, iBEC at ");
    say_hex(entry);
    say("\n");
    uint32_t count = 0;
    const AVPImagePatch *patches = patch_store_patches(PATCH_STAGE_IBEC, &count);
    uint8_t *base = (uint8_t *)entry;
    uint32_t i;

    /*
     * Only what the host uploaded, and usually that is the iBSS table.
     *
     * iBSS receives iBEC over USB, maps it out, and its handoff is diverted
     * here so this can patch it -- which is the whole reason the ROM is in this
     * path at all.  The table it needs is almost always the one already
     * uploaded for iBSS, because on this board the two stages are the same
     * binary; asking the host to send the same words twice was a step this did
     * not need.  So an iBEC table is an override, not a requirement, and with
     * none uploaded the iBSS one is used.
     *
     * The lookup is a static inline reading plain RAM, which is what this code
     * can do at all: MMU off, no stack to speak of, nothing outside .handoff
     * reachable.
     */
    if (patches == 0 || count == 0) {
        patches = patch_store_patches(PATCH_STAGE_IBSS, &count);
    }
    if (patches == 0 || count == 0) {
        return;
    }
    for (i = 0; i < count; i++) {
        uint8_t *at = base + patches[i].offset;

        adt_wr32(at, patches[i].instruction);
        adt_clean(at, at + 4);
    }
    __asm__ volatile("ic ialluis" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/*
 * The boot arguments the kernel is going to read, corrected here.
 *
 * iBoot does not pass on what it was given.  Measured: `setenv boot-args` with
 * "rd=md0 nand-enable-reformat=1 -progress serial=3 -v debug=0x2014e" reaches
 * the kernel as "rd=md0 -progress -restore" -- the debug arguments are dropped
 * and one of its own is added, which is a production loader's whitelist rather
 * than a truncation.  Nothing said so; the command line had to be read out of
 * boot_args in memory to find it.
 *
 * Without `serial=3` the kernel's serialmode has no SERIALMODE_OUTPUT bit, so
 * every other link in the chain -- the device the tree marks with
 * `boot-console`, `use-legacy-uart` defaulting on, the UART model itself --
 * is correct and useless.  Appending it here is after the whitelist and before
 * anything parses the line.
 */
/*
 * The memory controller's performance states, which this board has one of.
 *
 * ApplePMGR::initDriver counts the states each performance domain has, then
 * walks the pmgr device table, where a device may name the state it needs its
 * domain to be in.  Asking for a state the domain does not have is fatal:
 *
 *     ApplePMGR: virtual void ApplePMGR::initDriver(IOService *):1755
 *     CLPC-DCS-VNOM 11b request DCS.1 > 1 @ApplePMGR.cpp:1755
 *
 * The count comes from voltage-statesN, which iBoot writes rather than reads.
 * The tree ships four placeholder entries for the DCS rail and iBoot replaces
 * them with what it actually configured; it configures one.  The DRAM here is
 * never trained -- iBoot publishes memory-frequency as 24000000, the
 * reference clock, because nothing moves the DCS PLL off it -- so its
 * publisher enumerates one state, sees the next repeat it, and stops.
 *
 * Two devices vote on that domain, CLPC-DCS-VNOM for state 1 and
 * CLPC-DCS-VMAX for state 2, so three is the least that gets past both and
 * four is what the placeholder held and what iBoot's own publisher is told to
 * expect.  Every other domain on this tree comes out with enough of its own:
 * SOC four, AVE three, ECPU five, PCPU fifteen.  DCS is the only one short.
 *
 * The count can be forged without the entries.  For a domain of this kind it
 * is the number of entries when the first carries a voltage and the last
 * entry's index plus one when it does not, so a lone entry reading (3, 0)
 * answers four.  That leaves the kernel holding a count of four and a table
 * of one, and whatever reads a voltage for state one reads past the property.
 * Four entries that exist is the same answer without the trap.
 *
 * The tree cannot grow -- past its end is not mapped -- so the twenty-four
 * bytes come from inside it, the way the asmb records did.
 * manifest-properties is a pool of placeholders nobody reads, and one of its
 * sixty-four byte strings is shortened to forty.  That frees the space at the
 * end of the region running from the donor to voltage-states2, which slides
 * down onto it.  Shortening rather than dropping leaves the property count
 * alone.  Nothing past voltage-states2 moves, and the tree ends where it
 * ended.
 *
 * This is the only writer of voltage-states2: sptm_handoff_pmgr_states
 * carries no row for it, because a two-word stub there fails the size check
 * it makes and would put the single entry back.
 */
#define DCS_STATES      4u
#define DCS_ENTRY       8u
#define DCS_NEED        ((DCS_STATES - 1u) * DCS_ENTRY)
#define DCS_STEP        5u              /* as the other rails here step */

/*
 * Say the SMC's firmware is already up.
 *
 * /arm-io/smc/iop-smc-nub carries `pre-loaded`, and it reads zero, which tells
 * RTBuddy to load the coprocessor's firmware before talking to it.  There is
 * no firmware to load here -- the machine answers the mailbox itself -- so the
 * kernel has to be told the thing is already running, which is exactly what
 * Inferno's model sets on the same node when it builds the tree in userspace.
 *
 * The property already exists and is already four bytes wide, so this is a
 * value change and needs no room, unlike the DCS states below.
 *
 * Breadcrumb at scratch + 64: 0x5c00 on entry, 0x5c0f on success, and one of
 * the values between it for whichever step failed.
 */
void sptm_handoff_smc_preloaded(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_smc_preloaded(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 64);
    uint8_t *arm_io;
    uint8_t *smc;
    uint8_t *nub;
    uint8_t *prop;

    adt_wr32(mark, 0x5c00);
    arm_io = adt_child(root, "arm-io");
    if (arm_io == 0) {
        adt_wr32(mark, 0x5c01);
        return;
    }
    smc = adt_child(arm_io, "smc");
    if (smc == 0) {
        adt_wr32(mark, 0x5c02);
        return;
    }
    nub = adt_child(smc, "iop-smc-nub");
    if (nub == 0) {
        adt_wr32(mark, 0x5c03);
        return;
    }
    prop = adt_prop(nub, "pre-loaded");
    if (prop == 0 || adt_prop_size(prop) != 4) {
        adt_wr32(mark, 0x5c04);
        return;
    }
    adt_wr32(prop + ADT_PROP_HEADER, 1);
    adt_wr32(mark, 0x5c0f);
}

/*
 * The chip sends up a production AP, so runtime firmware validates.
 *
 * This property was forced to *zero* here, for the opposite reason: XNU maps
 * the CoreSight external-debug and UTT blocks only on a chip whose
 * /chosen/effective-production-status-ap is zero --
 *
 *     if (((i == CORESIGHT_ED) || (i == CORESIGHT_UTT)) &&
 *         !coresight_debug_enabled) continue;
 *
 * and `coresight_debug_enabled` comes from exactly that property
 * (kc:0xFFFFFFF00A36A80C raises it only when the property reads 0).  Left at
 * one, `cpu_data->coresight_base[UTT]` stays null, `ml_dbgwrap_halt_cpu`
 * returns DBGWRAP_ERR_UNSUPPORTED, and a panic on more than one running core
 * ends `cpu 1 failed to halt with error -5` and `!! debugger synchronization
 * failed, no stackshot !!`.
 *
 * What the zero cost, measured 2026-10-02: **every
 * runtime firmware this machine downloads is refused.**  AppleImage4 takes the
 * chip's production status from this same property -- kc:0xFFFFFFF009EEDAD8,
 * installed as the AP environment's getter -- and a payload's fusing policy
 * compares it against the payload's own 'EPRO' at kc:0xFFFFFFF009EF13B8,
 * `cmp(chipProduction & ~DPRO, EPRO)`.  Stock firmware is production-signed,
 * so at zero the comparison can never hold:
 *
 *     Image4: manifest evaluation: trust evaluation successful
 *     Image4: payload evaluation: payload type: 'mtpf'
 *     Image4: payload evaluation: signature evaluation successful
 *     Image4: payload evaluation: attempting fusing policy: 'EPRO'
 *     Image4: payload evaluation: 'EPRO' mismtch
 *     Image4: payload evaluation: failed production status check: 5011
 *     AIDImageDownloader::validationComplete() kFWValidationFailed: 5011
 *
 * -- the manifest and the signature both pass; only the fusing policy fails.
 * Four downloads failed that way in one boot: two AppleFirmwareUpdateKext,
 * and the two AIDImageDownloaders carrying `mtpf` and `mtfw`.  Without them
 * `AppleHIDTransportBootloaderRTBuddy`'s image never arrives, so the block it
 * registers on the downloader never runs `loadImage`, no protocol object is
 * ever built (`AppleHIDTransportDevice` + 176 stays null, which is what `No
 * protocol specified` reports), the dock channel is never enabled -- one read
 * of its interrupt window in a whole boot -- and there is no touch.
 *
 * One is also the *consistent* value.  The rest of the node describes a
 * production part on this build: `certificate-production-status` is one,
 * `security-domain` is one, `effective-security-mode-ap` is one, and the
 * manifest's own 'CPRO' matched.  The comment that used to stand here claimed
 * the node described a development part -- that was read off 19A348, and
 * 24A437's tree is not the same.  The zero was the field that disagreed with
 * the others, not the one that agreed.
 *
 * `debug-enabled` is a different property and this does not touch it.  It is
 * the only source of `debug_enabled` (kc:0xFFFFFFF00A9F2138) and therefore the
 * only thing `PE_i_can_has_debugger()` answers from, and it reads one here.
 * So the AMFI relaxations and ApplePMGR's perf-state gate do not depend on the
 * value changed here, whatever the older comments beside them say.
 *
 * To get the forced halt back for a boot, put this back to zero -- and then
 * no runtime firmware will load.  The narrow way, if both are ever wanted at
 * once, is to leave this at one and raise the kernel's own
 * `coresight_debug_enabled` byte instead: it is one byte in __DATA, and the
 * kc_force_* family already reaches that far.
 *
 * The property exists and is four bytes wide, so this is a value change and
 * needs no room.
 *
 * Breadcrumb at scratch + 164: 0x5e00 on entry, 0x5e0f on success.
 * (It used to be scratch + 68, which `sptm_handoff_ignore_devices` also
 * writes -- whichever ran last won and the other result was unreadable.)
 */
void sptm_handoff_production_status(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_production_status(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 164);
    uint8_t *chosen;
    uint8_t *prop;

    adt_wr32(mark, 0x5e00);
    chosen = adt_child(root, "chosen");
    if (chosen == 0) {
        adt_wr32(mark, 0x5e01);
        return;
    }
    prop = adt_prop(chosen, "effective-production-status-ap");
    if (prop == 0 || adt_prop_size(prop) != 4) {
        adt_wr32(mark, 0x5e02);
        return;
    }
    adt_wr32(prop + ADT_PROP_HEADER, 1);
    adt_wr32(mark, 0x5e0f);
}

/*
 * The blocks this board has no picture for.
 *
 * AppleM2ScalerCSCDriver matches arm-io/scaler0 because the tree names it,
 * starts, and dies inside its own start with a null:
 *
 *     Kernel data abort at pc 0x...  far 0x0                (LDR X0,[X19,#0x50]
 *                                                            then LDR X16,[X0])
 *     Kernel Extensions in backtrace: AppleM2ScalerCSCDriver
 *
 * The pointer at +0x50 is the hardware layer -- AppleM2ScalerCSCHalMSR11, for
 * the hardware-version of eleven the tree gives it -- and it was never built:
 * the whole boot touches the scaler's registers three times, one read of +0x0
 * and two writes of +0x4, and then faults.  There is no scaler behind them and
 * modelling one is a long way from what this board is for.
 *
 * IOKit has a way of being told not to look, and it is the tree's: a node
 * carrying `AAPL,ignore` is skipped by IODTFindMatchingEntries, which is what
 * createNubs walks, so the node is never published and nothing matches it.
 * Adding a property needs room the tree does not have, but renaming one costs
 * nothing -- the name is a fixed thirty-two byte field -- and `compatible` is
 * exactly the property whose absence is also wanted.  So it becomes the
 * marker, and the node loses the name drivers match on at the same time.
 *
 * `scaler0` was the first name in this list and is no longer here.  There is
 * a scaler behind those registers now -- the emulator models one -- and it
 * is not optional: CoreAnimation's `copy_surface` has only two
 * implementations, the scaler and a software render context that refuses the
 * ten-bit surfaces the window server hands it, so with this node masked every
 * swap was cancelled and the panel was black.  The model answers the version
 * the driver's extents table demands -- 0x000B0035, for this board's
 * hardware-version of eleven -- so the hardware layer is built and `start`
 * gets past the null pointer at +0x50 it used to die on.
 */
/*
 * `boot_args.CommandLine[]`: where it sits and how much room it has.
 * Both are argued for beside `sptm_handoff_boot_args`, which is the
 * other reader; they are facts about boot_args rather than about that
 * function, and the ignore pass needs them first.
 */
#define BA_COMMAND_LINE  0x6c
#define BA_COMMAND_LEN   512
#define BOOT_ARGS_EXTRA  0x00161000ULL

/* The one token that says this boot has a scaler behind `arm-io/scaler0`. */
static const char handoff_scaler_arg[] = "scaler0=1";
/*
 * And the one that says it has a JPEG codec behind `arm-io/jpeg0`
 * (the emulator's `jpeg-version=` machine option puts it there).  `scaler0`'s rule, for
 * `scaler0`'s reason: the node leaves the shared ignore list only for the
 * boots that ask, so 19A348 and 21A329 keep the mask they boot with.
 */
static const char handoff_jpeg_arg[] = "jpeg0=1";
/*
 * The opposite sense: `dcp` is left in the tree unless the boot says
 * `nodcp=1`.  Only a build whose display-coprocessor callback numbering the
 * DCP model does not know asks for it -- 23A341's restore kernel fails the
 * first callback of A401 start_signal ("callback failed with: 0x5",
 * link_protocol_priv.h:993) under both the 19A348 and the 24A437 maps
 * (measured on 23A341).  No other boot passes the argument, so
 * every other chain keeps its display.
 */
static const char handoff_nodcp_arg[] = "nodcp=1";
/*
 * The root's `baseband` node is renamed on every boot that runs the OS from
 * its own volume, so that tree has no `/baseband` at all, rather than one
 * whose `compatible` nobody matches.  There is no baseband behind this
 * board: no AppleBaseband driver ever attaches, so the installed OS sees a
 * board without cellular.
 *
 * A restore keeps the node.  restored's store_baseband_migration asks
 * BBUpdater which radio the board carries, and with no `/baseband` it fails
 * the restore (`BBUpdaterExtremeCreateCommon(): Unable to determine radio
 * vendor`, FAILURE:1005, 24A437 rec:job 20261021220527-52047); with the node
 * present every build's restore passes that step.  What tells the two apart
 * is the tree itself: iBoot names a `RAMDisk` range in /chosen/memory-map
 * only when it boots a ramdisk.
 *
 * launchd chooses CommCenter by that path: 23A341's
 * com.apple.CommCenter.plist carries `_LimitLoadToDeviceTree = baseband`
 * and com.apple.CommCenter-noncellular.plist (same Label) carries
 * `_LimitLoadFromDeviceTree = baseband`; 24A437 ships the same pair (its
 * launchd job cache holds com.apple.CommCenter-noncellular.plist and a
 * CommCenter job with `_LimitLoadFromDeviceTree = baseband`).  With the node present and masked,
 * the cellular CommCenter starts, waits for an AppleBaseband driver that
 * never attaches and aborts (23A341: `__TUAssertTrigger` in
 * TelephonyBasebandCreateController; 24A437: `std::runtime_error: Unable to
 * communicate with AppleBaseband driver`, 15 consecutive crashes), launchd
 * throttles it, and SpringBoard's main thread waits for ever in a
 * synchronous CoreTelephony call
 * (Research/Documents/Kernel/commcenter-baseband-abort-24a437.md,
 * rec:job 20261021211539-32603; with the node renamed,
 * rec:job 20261021214606-38478 reaches Hello).  This replaces the opt-in
 * `nobaseband=1`, which only 23A341's round passed.
 */

static const char *const handoff_ignored[] = {
    /*
     * The video decoder, which hangs the first system sleep.
     *
     * AppleAVD is a power-plane child of the root domain, so every sleep asks
     * it to change state.  `AppleAVD::setPowerState` goes to
     * `setPowerStateOnLegacy` -> `CAvdApComm::restoreM3context` ->
     * `CAvdMcpuSalviaA0::start`, which brings the decoder's own M3
     * microcontroller back up and then blocks waiting for it.  There is no
     * AVD here -- the tree names `avd` at 0x268000000 and `dart-avd` above it,
     * and neither has a device behind it -- so the wait never ends.
     *
     * What that costs, measured 2026-09-18: the power
     * button put the display to sleep, the root domain parked in
     * `kIOPM_SyncNotifyDidChange` with exactly one acknowledgement owed, and
     * thirty seconds later `IOService::watchdog_timer_expired` panicked with
     * `Sleep/Wake hang detected @IOPMrootDomain.cpp:12592`.  The owed ack was
     * found by scanning the guest's RAM for IOPowerConnection objects: 149 of
     * them, one with `awaitingAck` set, and its two ends were the root domain
     * and AppleAVD.
     *
     * Hardware video decode is the only thing lost, and nothing this board is
     * for uses it.
     */
    "avd",
    /*
     * The touch processor, which the restore stops on:
     *
     *     Update[AHT] error - update_aht_block_invoke: mtp-transport update
     *     failed with code 0x00000001
     *
     * /arm-io/dockchannel-mtp carries mtp-transport and, under it,
     * multi-touch.  restored reads MtpFirmware and Multitouch back out of the
     * filesystem it has just written -- both arrive -- and then pushes them at
     * a processor that is not here.  That is the last step of
     * update_device_firmware, so the whole install fails at thirty-three per
     * cent with the image already on the NAND.
     *
     * Inferno models no touch processor either -- and its own touch controller
     * is a z2 on SPI, which is the A13 design; d16 puts multi-touch behind the
     * MTP processor and a dock channel instead, so there is nothing to borrow.
     *
     * Unmasked since 2026-09-07, while that processor is being modelled.  The
     * transport is the same FIFO block as dockchannel-uart and the protocol is
     * the one Asahi's dockchannel-hid implements; the emulator answers the four
     * register windows.  Put it back in this list before restoring again --
     * update_device_firmware still pushes MtpFirmware and Multitouch at it.
     */
    /* "dockchannel-mtp", */
    /*
     * The camera, which checks the silicon before anything else:
     *
     *     "ISP version from platform struct does not match rISP_ISPVERSION
     *      register value"  @AppleH13CamIn.cpp:3090
     *
     * There is no sensor, no lens and no image signal processor here, and a
     * version register answered out of a table would only move the argument
     * one line further down.
     */
    "isp",
    /*
     * The rest of the same family, for the same reason: nothing to accelerate.
     * `jpeg0` is masked below instead, unless the boot carries `jpeg0=1`.
     */
    "jpeg1",
    "ave",
    "ane",
    /*
     * /arm-io/pearl-sep (Face ID) is not masked: d16 has Face ID, and the
     * software reads the node, not the driver, to decide that it has.
     * 17.0's and later launchd jobs for biometrickitd carry
     * `_LimitLoadToDeviceTree = pearl-sep`; with the node masked the job is
     * never loaded, every lookup of com.apple.pearld fails "No such
     * process", and SpringBoard's main thread waits for ever in a
     * synchronous BiometricKit call, with Setup waiting on SpringBoard
     * (Research/Documents/Kernel/pearl-driver-on-sprl-endpoint-21a329.md,
     * rec:job 20261021191016-84170).  What keeps the missing sensor from
     * being asked anything is the kernel side: a build whose
     * ApplePearlSEPDriver starts names `kernel.pearl_sep_driver_absent` in
     * its patch table.  This replaces the old opt-in `pearl-sep=1`, which
     * kept the node only for 20A362 and 23A341.  The mask it replaces was
     * added for a driver that matches the node and then fails every call
     * (`isPearlDisabledSpecific: ... failed`, and on 24A437
     * `ApplePearlUserClient::extPerform ... err 0xe00002d6` before launchd
     * rebooted for SpringBoard); a build that shows that again needs the
     * kernel row, not the mask.
     */
    /*
     * The graphics coprocessor, which will not be hurried:
     *
     *     panic(cpu 3 ...): "RTBuddy(GFX)::_performPowerStateChangeGated "
     *                       "No response received in 20s
     *
     * Its nub is described as a coprocessor that starts -- pre-loaded, a
     * firmware-name of GFX, segment-names and segment-ranges -- so RTBuddy
     * brings it up and then waits twenty seconds for a power-state
     * acknowledgement that nothing here sends.  The same shape as the SMC
     * mailbox before it, and that one was answered by modelling the endpoint;
     * AGX is a great deal more than an endpoint, and none of it is on the way
     * to a userspace that runs.  Ignored, so nothing matches it at all.
     *
     * (The tree has another way to say this -- iop-mtp-nub carries
     * `disable = 1` and RTBuddy reports "IOP start disabled by device tree" --
     * which would leave the node published and only stop the coprocessor
     * starting.  It needs a property this node does not have room for, and
     * nothing here wants the node.)
     */
    /*
     * The graphics coprocessor.  Unmasked 2026-09-08: the emulator answers its
     * mailbox (an `iop,ascwrap-v4` like smc, ans, dcp and mtp) and the sgx
     * register block the accelerator reads its chip configuration from, so
     * the twenty-second power-state timeout above is no longer the whole
     * story -- what AGX does after it starts is now the thing to find out,
     * and it cannot be found out with the node hidden.
     *
     * Masked again 2026-09-18, for the software-rendering configuration.
     * `AGXMetalG14.bundle` is moved aside on that disk, so nothing in userland
     * asks the accelerator for anything -- but the kext still owns the node,
     * it is still a power-plane child of the root domain, and the first idle
     * sleep asks it to acknowledge a state change it never will:
     *
     *     panic(cpu 0 ...): "Power state change to Sleep did not complete!"
     *                       @agxp_kext_rtbuddy.cpp:398
     *
     * which ended two boots, after four and seven minutes of idling.  It is
     * exactly the shape of the `avd` entry above.
     *
     * **Uncommenting this ends the AGX track.**  With the node hidden
     * `AGXAcceleratorG14P::probe` fails and the emulator's AGX command-stream
     * translation has nothing to translate for -- Inferno's arrangement, not this tree's ambition.
     * Comment it back out for a GPU boot.
     *
     * Commented out 2026-10-03 for the 19A348 GPU boot that captures the
     * AIR every pipeline compile is handed.
     */
    /* "gfx-asc", */
    /*
     * And the accelerator's own node.
     *
     * Masking the coprocessor above was meant to stop AGX matching at
     * all, and it does not: `AGXAcceleratorG14P` matches on this node
     * and then waits for the firmware bundle the coprocessor would
     * have served, saying so every ten seconds for the life of the
     * machine --
     *
     *     AGXk: AGXAcceleratorG14P_A0: Cannot find firmware
     *     AGXArmFirmwareChinookV17 payload kext
     *     'AGXFirmwareKextRTBuddy - AGXFirmwareKextG14RTBuddy'
     *     after 10 seconds. Trying again...
     *
     * Measured 2026-09-21 on the installed 19A348 chain, which is
     * already on the software-rendering disk.  The ROM's own two uses
     * of this node -- the `gfx-data-size` bump and the
     * `gpu-idle-off-delay-ms` check -- both test it for NULL first, so
     * hiding it costs them nothing.
     *
     * Comment this back out for a GPU boot, the same as `gfx-asc`.
     * Commented out 2026-10-03 with it.
     */
    /* "sgx", */
    /*
     * The always-on processor.  Its mailbox is not modelled, so RTBuddy waits
     * the full timeout and then takes the machine down rather than leaving the
     * block unstarted:
     *
     *     RTBuddy(AOP)::_setManagedStateGated
     *     "No response received in 20s, AOP not started? (4)"
     *
     * Intermittent only because twenty seconds is a race against the rest of
     * the boot.  What hangs off it -- jarvis, als, accel, gyro, compass -- is
     * sensors, and a restore over USB asks none of them.
     */
    "aop",
    /*
     * The power-management processor, which is the same shape again:
     *
     *     RTBuddy(PMP)::_setManagedStateGated
     *     "No response received in 20s, PMP not started? (4)"
     *
     * It was there all along and could not be seen.  Until the virtual
     * timer's compare value was corrected in `hvf_wfi`, a core that halted
     * with only that timer armed slept for about nine days, so no timeout in
     * the kernel ever expired: the log simply stopped at
     * `RTBuddy(PMP): Resuming...` and the machine idled.  With deadlines
     * arriving on time the wait ends the way it always should have, in a
     * panic, and the honest description is the same as for gfx-asc and aop --
     * this machine has no such coprocessor and nothing on the way to a
     * restore asks it anything.
     */
    "pmp",
    /*
     * And the rest of the same family, found the same way.
     *
     * With deadlines arriving on time every unanswered RTKit coprocessor
     * reaches its twenty-second timeout and takes the machine down in turn --
     * PMP first, then SIO -- so they are dealt with together rather than one
     * boot at a time.  Six appear in a boot's log: ANS2 and SMC are modelled
     * and answer; these three are not, and none of them is on the way to a
     * restore over USB.  DCP drives a display this machine does not have, MTP
     * the touch processor whose dockchannel is already out of the tree above,
     * and SIO the audio and serial DMA engine.
     */
    "sio",
    /* dcp stays: the emulator models it */
    /*
     * The touch processor.  Unmasked 2026-09-07 together with
     * dockchannel-mtp above: it is an `iop,ascwrap-v4` like smc, ans and dcp,
     * and the emulator answers its mailbox now.
     *
     * It has to be here rather than left out with the other quiet ones,
     * because it is not the channel that gates touch --
     * `AppleHIDTransportDeviceFIFO::handleStart` asks for an IOSlaveProcessor
     * whose role is "MTP" and returns false without a word when there is
     * none.  With this masked the node's `compatible` is renamed to
     * AAPL,ignore, AppleASCWrapV4 never matches it, no RTBuddy nub is
     * published, and the transport above gives up silently -- which is what
     * a whole boot of one register access on the dock channel looked like.
     */
    /* "mtp", */
};

/*
 * The same, for nodes that hang off the root rather than off arm-io.
 */
static const char *const handoff_ignored_root[] = {
    /*
     * The cellular baseband, which waits for a sibling that is not there:
     *
     *     AppleBasebandM20::setupSPMIServiceGated:
     *         Failed waiting for AppleBasebandSPMIHEB
     *     AppleBasebandM20::radioOnGated:
     *         failed to drive BB_PMU_PON pin HIGH, error: 0xe00002c7
     *
     * It is the only driver left in a boot's log that waits rather than
     * declining -- everything else that has nothing behind it says "probe
     * fails" and gets out of the way.  There is no modem here and there will
     * not be one; a device the machine does not model and that blocks instead
     * of failing is a device to take out of the tree.
     */
    "baseband",
};

/*
 * The storage processor's firmware, described the way iBoot describes the
 * others'.
 *
 * RTBuddy will not start a coprocessor it has no firmware for.  What it wants
 * is what iBoot leaves on the nub after loading one: pre-loaded, segment-names
 * and segment-ranges.  iop-gfx-nub carries only a firmware-name in the board's
 * own ADT and carries all three by the time the kernel sees the tree; iBoot
 * added them.  iop-ans-nub carries none of them in either tree and iBoot adds
 * nothing, because a boot that arrives over USB never needs the NAND and never
 * starts the processor in front of it -- a start callback on this mailbox
 * confirmed it, over a whole boot neither stage wrote CPU_CONTROL once.
 *
 * So the description is written here.  There is no firmware to point it at and
 * none is needed: the mailbox is answered by the machine model, and the core is
 * already running by the time the kernel looks.  What the description has to do
 * is be well-formed enough that RTBuddy announces it and starts talking, which
 * is what publishes ANS2Endpoint1 -- the service
 * AppleANS2NVMeController::start waits on with no timeout, taking IONVRAM and
 * every driver that writes NVRAM down with it.
 *
 * Growing a property means moving every byte after it, since a property's
 * length is what says where the next one begins.  The tree has room: it ends
 * well short of the 0x40000 the memory map gives it.
 */
#define ADT_TREE_CAPACITY       0x40000u

/*
 * How big the tree is really allowed to get.
 *
 * ADT_TREE_CAPACITY is what the older builds' trees are given, and the helpers
 * below took it for the truth.  24A437's tree is 0x413d8 bytes in a region the
 * boot arguments give as 0x44000, so every step that had to grow it was
 * refused for room that exists -- and the refusal is silent: the step leaves
 * its breadcrumb and the boot carries on.  Read off a live boot's scratch
 * page, 2026-09-30: +208 held 0x44000 the whole time, while +112 held 0xdc93
 * (the DCP firmware description never went on `iop-dcp-nub`) and +72 held
 * 0xa5e2 (nor did the ANS one).  The first of those is the whole display:
 * RTBuddy::_attemptFirmwareLoad has nothing to load, so
 * AppleA7IOP::startCPUWithOptions is never called and the coprocessor is never
 * released -- `cpu-run-timestamp` 0, CPU_CTRL 0, not one access to its AKF
 * window -- and IOMobileFramebufferAP waits for a link no rollcall can offer.
 *
 * `sptm_handoff_run` writes the real length to the scratch page before the
 * first edit runs, so that word is the truth.  The page's address is SCRATCH
 * in sptm_handoff.S, known here the same way rom_real_dcp_firmware knows
 * BOOT_ARGS_EXTRA.
 *
 * Clamped both ways.  Never below the constant, so a build whose own length is
 * smaller behaves exactly as it did before this -- 19A348 is unchanged by
 * construction.  Never above a megabyte, so a word that has not been written
 * cannot authorise a scribble past the tree.
 */
#define ADT_SCRATCH_BASE        0x00160000ULL   /* SCRATCH, sptm_handoff.S */
#define ADT_SCRATCH_TREE_LEN    208u            /* sptm_handoff_run leaves it */
#define ADT_TREE_CAPACITY_MAX   0x100000u

HANDOFF static uint32_t adt_tree_capacity(void)
{
    const volatile uint32_t *len =
        (const volatile uint32_t *)(ADT_SCRATCH_BASE + ADT_SCRATCH_TREE_LEN);
    uint32_t cap = *len;

    if (cap < ADT_TREE_CAPACITY || cap > ADT_TREE_CAPACITY_MAX) {
        return ADT_TREE_CAPACITY;
    }
    return cap;
}

/*
 * Whether this boot is running Apple's own dcpf.
 *
 * Read twice -- the DCP's own handoff and vram's, which carves the memory for
 * it -- and there is no state between them, so it is a function.  The host
 * leaves its boot-args at BOOT_ARGS_EXTRA behind eight magic bytes, and the
 * bytes after them are a line with terminators in it, so this scans by length
 * rather than stopping at the first NUL.
 */
HANDOFF static int rom_real_dcp_firmware(void)
{
    static const char want[] = "dcp-real-firmware";
    const volatile char *extra = (const volatile char *)0x00161000ULL;
    unsigned i, j;

    for (i = 8; i + sizeof(want) <= 512; i++) {
        for (j = 0; j < sizeof(want) - 1; j++) {
            if (extra[i + j] != want[j]) {
                break;
            }
        }
        if (j == sizeof(want) - 1) {
            return 1;
        }
    }
    return 0;
}

/* A megabyte of DRAM below the panic region, which nothing else claims. */
#define ANS_FW_BASE             0x8FFC00000ULL
#define ANS_FW_TEXT_SIZE        0x57000ULL
#define ANS_FW_DATA_SIZE        0xa9000ULL

HANDOFF static void adt_rename_prop(
    uint8_t *prop,
    const char *name,
    unsigned length
)
{
    unsigned c;

    for (c = 0; c < 32u; c++) {
        prop[c] = (uint8_t)(c < length ? name[c] : 0);
    }
}

HANDOFF static void adt_wr64(uint8_t *p, uint64_t v)
{
    adt_wr32(p, (uint32_t)v);
    adt_wr32(p + 4, (uint32_t)(v >> 32));
}

/*
 * Give a property a new length, moving the rest of the tree out of its way.
 * Returns 0 if there is no room, in which case nothing has been touched.
 */
HANDOFF static int adt_resize_prop(uint8_t *root, uint8_t *prop, uint32_t want)
{
    uint32_t have = adt_prop_size(prop);
    uint32_t old_span = (have + 3u) & ~3u;
    uint32_t new_span = (want + 3u) & ~3u;
    uint8_t *end = adt_node_end(root);
    uint8_t *from = prop + ADT_PROP_HEADER + old_span;
    uint8_t *p;
    uint32_t delta;
    uint32_t c;

    if (new_span == old_span) {
        adt_wr32(prop + 32, want);
        return 1;
    }
    if (new_span < old_span) {
        delta = old_span - new_span;
        for (p = from; p < end; p++) {
            *(p - delta) = *p;
        }
        adt_wr32(prop + 32, want);
        return 1;
    }
    delta = new_span - old_span;
    if ((uint32_t)((end - root) + delta) > adt_tree_capacity()) {
        return 0;
    }
    /* back to front, so the move does not overwrite what it has yet to read */
    p = end;
    while (p > from) {
        p--;
        p[delta] = *p;
    }
    for (c = old_span; c < new_span; c++) {
        prop[ADT_PROP_HEADER + c] = 0;
    }
    adt_wr32(prop + 32, want);
    return 1;
}

/*
 * Append a four-byte property to a node.
 *
 * adt_resize_prop can only reshape a property that is already there, which is
 * why every description written here so far has been carved out of a spare
 * one.  iop-ans-nub has eight properties and no spare left, so the last thing
 * it needs is added rather than renamed: the bytes from the end of the node's
 * property block to the end of the tree move up by one property, the header
 * goes in the gap, and the node's property count grows by one.
 */
/*
 * Append a one-word property, told how big the tree is allowed to get.
 *
 * ADT_TREE_CAPACITY is the size the older builds' trees are given.  27.0's is
 * larger -- the boot arguments carry 0x44000 -- and sptm_handoff_run leaves
 * that length at scratch + 208 for the steps that need it.  Measured
 * 2026-09-28: the 24A437 tree is already past 0x40000, so an add that trusts
 * the constant is refused for room that exists, and a silent refusal looks
 * exactly like a step that found nothing to do.  `adt_add_prop` keeps the
 * constant so every caller that has not been given the real length behaves
 * exactly as before, and the cap is never allowed below it.
 */
HANDOFF static int adt_add_prop_cap(
    uint8_t *root,
    uint8_t *node,
    const char *name,
    unsigned length,
    uint32_t value,
    uint32_t cap
)
{
    const uint32_t span = ADT_PROP_HEADER + 4u;
    uint8_t *at = adt_after_props(node);
    uint8_t *end = adt_node_end(root);
    uint8_t *p;

    if (cap < ADT_TREE_CAPACITY) {
        cap = ADT_TREE_CAPACITY;
    }
    if ((uint32_t)((end - root) + span) > cap) {
        return 0;
    }
    /* back to front, so the move does not overwrite what it has yet to read */
    p = end;
    while (p > at) {
        p--;
        p[span] = *p;
    }
    adt_rename_prop(at, name, length);
    adt_wr32(at + 32, 4);
    adt_wr32(at + ADT_PROP_HEADER, value);
    adt_wr32(node, adt_rd32(node) + 1u);
    return 1;
}

HANDOFF static int adt_add_prop(
    uint8_t *root,
    uint8_t *node,
    const char *name,
    unsigned length,
    uint32_t value
)
{
    return adt_add_prop_cap(
        root,
        node,
        name,
        length,
        value,
        adt_tree_capacity()
    );
}

/*
 * Append a property of any length to a node: adt_add_prop for more than one
 * word.  The data is padded to a word, as every property's is.
 */
HANDOFF static int adt_add_prop_bytes(
    uint8_t *root,
    uint8_t *node,
    const char *name,
    unsigned length,
    const uint8_t *data,
    uint32_t size
)
{
    const uint32_t padded = (size + 3u) & ~3u;
    const uint32_t span = ADT_PROP_HEADER + padded;
    uint8_t *at = adt_after_props(node);
    uint8_t *end = adt_node_end(root);
    uint8_t *p;
    uint32_t c;

    if ((uint32_t)((end - root) + span) > adt_tree_capacity()) {
        return 0;
    }
    p = end;
    while (p > at) {
        p--;
        p[span] = *p;
    }
    adt_rename_prop(at, name, length);
    adt_wr32(at + 32, size);
    for (c = 0; c < padded; c++) {
        at[ADT_PROP_HEADER + c] = c < size ? data[c] : 0;
    }
    adt_wr32(node, adt_rd32(node) + 1u);
    return 1;
}

/*
 * The display processor's firmware, described the same way.
 *
 * With /arm-io/dcp no longer ignored, RTBuddy(DCP) prints its start line and
 * then waits for a firmware service that never comes, exactly as ANS2 did
 * before its nub was dressed: iop-dcp-nub carries no-firmware-service and
 * quiesced but neither pre-loaded nor running, and no segment description.
 * The five properties below are what iBoot leaves on it after loading
 * RestoreDCP.  There is no image behind the ranges here either; the DCP is a
 * device model in the emulator whose greeting is ready the moment the
 * kernel starts the core, which is what `running` says.
 */
#define DCP_FW_BASE             0x8FFB00000ULL
#define DCP_FW_TEXT_SIZE        0x80000ULL
#define DCP_FW_DATA_SIZE        0x80000ULL

/*
 * And where Apple's own firmware lives; see dcp_carveout.
 *
 * `DCP_FW_REAL_BASE` is `region-id-24`'s end less VRAM_WANT less
 * DCP_FW_REAL_SIZE, for this board and this iBoot: 0x801D20000 + 0xF42CC000
 * - 0x1000000 - 0x1000000.  sptm_handoff_vram recomputes it and says so if it
 * ever differs.  **It and the emulator DCP model's firmware load base must
 * name the same address**: the firmware
 * executes out of these pages and the AP reads its patchbay and its shared
 * buffers through them, so a disagreement is a core running on memory nobody
 * else can see.
 *
 * `DCP_FW_REAL_TEXT` is dcpf's own __TEXT, 19A348's being 0x1ce000 with
 * __DATA at 0x1ce000 behind it; the rest of the region is the heap the
 * patchbay's RHB1 points at, and it is described as part of __DATA because
 * what the AP needs from these records is the IOP-address-to-physical
 * relation, which is the same for all of it.
 */
#define DCP_FW_REAL_BASE        0x8F3FEC000ULL
#define DCP_FW_REAL_SIZE        0x1000000ULL
#define DCP_FW_REAL_TEXT        0x1ce000ULL
#define DCP_FW_REAL_DATA        (DCP_FW_REAL_SIZE - DCP_FW_REAL_TEXT)

/* Defined further down; used by the stand-down report. */
HANDOFF static void say_node_props(uint8_t *node, const char *label);

/*
 * The TZ taggers iBoot's DCP load would have locked.
 *
 * A t8110 DART whose PARAMS2 (+0x4) has bit 2 has a TZ tagger, and the
 * kernel refuses one that arrives unlocked: 20A362's
 * `t8110dart` verify (kc16 0xfffffff008497b24) wants WRITE_DISABLE|LOCK
 * (0x18) in +0x208 and +0x200 and panics "SECURITY VIOLATION: instance %u
 * WRITE_DISABLE enabled expected 0x18 actual 0" otherwise; 23A341's SPTM
 * makes the same test.  iBoot sets them when it brings the DCP up and binds
 * its TZ carve-outs (ida:ibec26 0x1fc0fa15c, flags bits 2 and 3; ida:ibec16
 * 0x1fc0e57d4/0x1fc0e57fc).  On an installed boot that is the DCP image
 * iBoot reads out of EAN (BuildManifest DCP IsiBootEANFirmware).  Here iBoot
 * does not load it -- this function is reached only when the ROM has to
 * describe the firmware itself -- so nothing locks the display DARTs, and
 * the ROM leaves them as iBoot's DCP load would have.
 *
 * Which DARTs is the hardware's answer, not a list: every DART instance the
 * tree names (`instance` entry 'TRAD') whose PARAMS2 says it has a tagger
 * and whose words are not locked yet.  The bits are ORed in, so what iBoot
 * did write (an APF bit 1, a lock it did set) stays.  The TZ regions are not
 * programmed: the kernel compares them with the tree's TZT configuration,
 * and the tree describes none for a DCP iBoot did not load.
 */
#define T8110_DART_PARAMS2      0x004u
#define T8110_DART_HAS_TZT      (1u << 2)
#define T8110_DART_TZ_LOCKED    0x18u   /* WRITE_DISABLE | LOCK */

HANDOFF static uint64_t arm_io_physical(uint8_t *arm_io, uint64_t addr)
{
    uint8_t *ranges = adt_prop(arm_io, "ranges");
    uint32_t size;

    if (ranges == 0) {
        return 0;
    }
    size = adt_prop_size(ranges);
    for (uint32_t off = 0; off + 24u <= size; off += 24u) {
        const uint8_t *r = ranges + ADT_PROP_HEADER + off;
        uint64_t child = adt_rd32(r) | ((uint64_t)adt_rd32(r + 4) << 32);
        uint64_t parent = adt_rd32(r + 8) | ((uint64_t)adt_rd32(r + 12) << 32);
        uint64_t length = adt_rd32(r + 16) | ((uint64_t)adt_rd32(r + 20) << 32);

        if (addr >= child && addr - child < length) {
            return parent + (addr - child);
        }
    }
    return 0;
}

HANDOFF static void dart_lock_taggers(uint8_t *root)
{
    static const char compat[] = "dart,t8110";
    uint8_t *arm_io = adt_child(root, "arm-io");
    uint8_t *node;
    uint32_t children;

    if (arm_io == 0) {
        return;
    }
    children = adt_rd32(arm_io + 4);
    node = adt_after_props(arm_io);
    for (uint32_t i = 0; i < children; i++, node = adt_node_end(node)) {
        uint8_t *c = adt_prop(node, "compatible");
        uint8_t *reg = adt_prop(node, "reg");
        uint8_t *inst = adt_prop(node, "instance");
        uint32_t n;

        if (c == 0 || reg == 0 || inst == 0 ||
            adt_prop_size(c) < sizeof(compat)) {
            continue;
        }
        n = 0;
        while (n < sizeof(compat) && c[ADT_PROP_HEADER + n] == (uint8_t)compat[n]) {
            n++;
        }
        if (n != sizeof(compat)) {
            continue;
        }
        n = adt_prop_size(reg) / 16u;
        if (adt_prop_size(inst) / 16u < n) {
            n = adt_prop_size(inst) / 16u;
        }
        for (uint32_t k = 0; k < n; k++) {
            const uint8_t *tag = inst + ADT_PROP_HEADER + 16u * k;
            const uint8_t *r = reg + ADT_PROP_HEADER + 16u * k;
            uint64_t base;
            volatile uint32_t *dart;

            if (tag[0] != 'T' || tag[1] != 'R' || tag[2] != 'A' || tag[3] != 'D') {
                continue;
            }
            base = arm_io_physical(arm_io, adt_rd32(r) |
                                   ((uint64_t)adt_rd32(r + 4) << 32));
            if (base == 0) {
                continue;
            }
            dart = (volatile uint32_t *)base;
            if ((dart[T8110_DART_PARAMS2 / 4u] & T8110_DART_HAS_TZT) == 0) {
                continue;
            }
            if ((dart[0x200u / 4u] & T8110_DART_TZ_LOCKED) == T8110_DART_TZ_LOCKED &&
                (dart[0x208u / 4u] & T8110_DART_TZ_LOCKED) == T8110_DART_TZ_LOCKED) {
                continue;
            }
            dart[0x200u / 4u] |= T8110_DART_TZ_LOCKED;
            dart[0x208u / 4u] |= T8110_DART_TZ_LOCKED;
            say("OpenBootROM: TZ tagger locked at ");
            say_hex(base);
            say("\n");
        }
    }
}

void sptm_handoff_dcp_firmware(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_dcp_firmware(uint64_t tree, uint64_t scratch)
{
    static const char preloaded[] = "pre-loaded";
    static const char seg_names[] = "segment-names";
    static const char seg_ranges[] = "segment-ranges";
    static const char text_data[] = "__TEXT;__DATA";
    static const char fw_name[] = "firmware-name";
    static const char running[] = "running";
    static const uint8_t dcp_name[8] = { 'D', 'C', 'P', 0, 0, 0, 0, 0 };
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 112);
    uint8_t ranges[64];
    uint8_t *nub;
    uint8_t *own2 = (uint8_t *)(scratch + 164);
    int real_firmware = 0;

    adt_wr32(mark, 0xdc90);
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "dcp"), "iop-dcp-nub");
    if (nub == 0) {
        adt_wr32(mark, 0xdc91);
        return;
    }
    /*
     * A boot running Apple's own dcpf says so in its boot-args, and then the
     * kernel's firmware service has to be allowed to run; see
     * rom_real_dcp_firmware.
     */
    {
        static const char nofw[] = "no-firmware-service";
        /*
         * Slot 160 is free -- every mark in this file is between 24 and 132,
         * and mark + 4 is another function's own (scratch + 116), which is
         * why the first attempt read back untouched.
         */
        uint8_t *own = (uint8_t *)(scratch + 160);
        int found = rom_real_dcp_firmware();
        /*
         * Its own word, four past the shared mark, because the marks below
         * are written after this and would bury it: 1 = the boot-arg was not
         * there, 2 = it was but the property was not, 3 = renamed.
         */
        adt_wr32(own, found ? 2 : 1);
        if (found) {
            uint8_t *p = adt_prop(nub, nofw);

            if (p != 0) {
                /* One byte, and the name no longer matches. */
                *p = (uint8_t)'x';
                adt_wr32(own, 3);
            }
            /*
             * `quiesced` stays.  Renaming it makes the kernel start the
             * coprocessor instead of resuming it, and it then waits twenty
             * seconds in `_setManagedStateGated` for a hello from a core
             * iBoot has already brought up and handshaken with.  The
             * pre-loaded path that sets `iBootLoaded` runs from
             * `RTBuddy::start` either way.
             */
        }
        real_firmware = found;
    }
    /*
     * Already dressed?  Then iBoot loaded the firmware and this has nothing
     * to add; see rom_standdown.
     */
    {
        static const char seg_ranges_probe[] = "segment-ranges";

        if (adt_prop(nub, seg_ranges_probe) != 0) {
            adt_wr32(mark, 0xdc9f);
            say("OpenBootROM: iop-dcp-nub already carries segment-ranges;"
                " leaving it to whoever loaded the firmware\n");
            /*
             * And what else is on it.  `RTBuddy::_initConfigEDT` takes the
             * kernel's `pre-loaded` straight from this node, and
             * `RTBuddyFirmware::preloadedFirmware` is the only thing that
             * sets `iBootLoaded` -- which is the byte the display firmware's
             * `start_ios_gated` reads.
             */
            say_node_props(nub, "iop-dcp-nub");
            return;
        }
        say("OpenBootROM: iop-dcp-nub carries no segment-ranges; describing"
            " the firmware here\n");
        dart_lock_taggers(root);
    }
    /* Ignored above?  Then its compatible is gone and so is the driver. */
    if (adt_prop(adt_child(adt_child(root, "arm-io"), "dcp"), "AAPL,ignore") != 0) {
        adt_wr32(mark, 0xdc92);
        return;
    }
    if (adt_add_prop(root, nub, preloaded, sizeof(preloaded), 1) == 0) {
        adt_wr32(mark, 0xdc93);
        return;
    }
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "dcp"), "iop-dcp-nub");
    if (adt_add_prop_bytes(root, nub, seg_names, sizeof(seg_names),
                           (const uint8_t *)text_data, sizeof(text_data)) == 0) {
        adt_wr32(mark, 0xdc94);
        return;
    }
    {
        /*
         * {physical, IOP, physical, size | flags}, 32 bytes a segment; the
         * kernel reads them in RTBuddy::getSegmentMap, one name from
         * `segment-names` each.  A boot with Apple's firmware describes the
         * real image and the heap behind it; see dcp_carveout.
         */
        uint64_t base = real_firmware ? DCP_FW_REAL_BASE : DCP_FW_BASE;
        uint64_t text = real_firmware ? DCP_FW_REAL_TEXT : DCP_FW_TEXT_SIZE;
        uint64_t data = real_firmware ? DCP_FW_REAL_DATA : DCP_FW_DATA_SIZE;
        /*
         * The third word is what the coprocessor calls the address; see
         * seg_remap.  Its image is mapped at its own 0, so that is an offset
         * into the region.  The model's records keep the value they have
         * always had -- nothing translates them.
         */
        uint64_t iop_text = real_firmware ? 0 : base;
        uint64_t iop_data = real_firmware ? text : base + text;

        adt_wr64(ranges + 0,  base);
        adt_wr64(ranges + 8,  0);
        adt_wr64(ranges + 16, iop_text);
        adt_wr64(ranges + 24, (1ULL << 32) | text);
        adt_wr64(ranges + 32, base + text);
        adt_wr64(ranges + 40, text);
        adt_wr64(ranges + 48, iop_data);
        adt_wr64(ranges + 56, data);
    }
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "dcp"), "iop-dcp-nub");
    if (adt_add_prop_bytes(root, nub, seg_ranges, sizeof(seg_ranges), ranges,
                           sizeof(ranges)) == 0) {
        adt_wr32(mark, 0xdc95);
        return;
    }
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "dcp"), "iop-dcp-nub");
    if (adt_add_prop_bytes(root, nub, fw_name, sizeof(fw_name), dcp_name,
                           sizeof(dcp_name)) == 0) {
        adt_wr32(mark, 0xdc96);
        return;
    }
    if (real_firmware) {
        /*
         * Loaded, not running: `running` sets the same flag inside RTBuddy
         * that `no-firmware-service` does (_initConfigEDT writes
         * `+8738 = running || no-firmware-service`), so adding it here would
         * undo the rename above.  It also gates updateCoredumpWithPatchBay.
         */
        adt_wr32(own2, 4);
        return;
    }
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "dcp"), "iop-dcp-nub");
    if (adt_add_prop(root, nub, running, sizeof(running), 1) == 0) {
        adt_wr32(mark, 0xdc97);
        return;
    }
    adt_wr32(mark, 0xdcff);
}

/*
 * The display's power, kept on the AP's side of the mailbox.
 *
 * From 21A329 on, the tree says `/arm-io/dcp0-expert/remote-power-state = 1`
 * (19A348 and 20A362 do not carry the property; 21A329, 22G100 and 24A437
 * all do).  It tells AppleDCPExpert::start that the DCP firmware owns its own
 * power state: the driver then waits for the firmware to publish an
 * AFKEndpointInterface named `dcpexpert-service` and keeps it at +0x160, and
 * every later _changeDCPPowerStateGated sends that service command 193
 * through it (_setDCPExpertPowerState, kc17 0xfffffff02898ed44).  The model
 * of the DCP in the emulator is the IOMFB endpoint and nothing else;
 * no `dcpexpert-service` is ever published, so +0x160 stays NULL, and 21A329
 * loads through it the first time the display goes down for a system sleep:
 *
 *     panic: Kernel data abort ... pc 0xfffffff06d4d6dac (kc17
 *       0xfffffff02898edac, LDR X16,[X23] with X23 = +0x160 = 0)
 *       esr 0x96000005 far 0 -- every frame in com.apple.driver.AppleDCP
 *
 * in every 21A329 boot that idled to sleep or took a power press (four of
 * them).
 *
 * Zero is the 19A348/20A362 behaviour, which sleeps and wakes: the expert
 * drives the coprocessor's power through RTBuddy itself.  The kernel reads it
 * as `remote-power-state != 0` (kc17 0xfffffff02898ced8), and its own
 * `dcp-remote-power-state` boot-arg would say the same, but the boot-args have
 * no room left.  A boot running Apple's dcpf leaves it alone: that firmware
 * publishes the service itself.
 */
void sptm_handoff_dcp_local_power(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_dcp_local_power(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    /* Slot 172 is free: the marks in this file skip from 168 to 176. */
    uint8_t *mark = (uint8_t *)(scratch + 172);
    uint8_t *expert;
    uint8_t *prop;

    adt_wr32(mark, 0xdd00);
    if (rom_real_dcp_firmware()) {
        adt_wr32(mark, 0xdd01);
        return;
    }
    expert = adt_child(adt_child(root, "arm-io"), "dcp0-expert");
    if (expert == 0) {
        adt_wr32(mark, 0xdd02);
        return;
    }
    prop = adt_prop(expert, "remote-power-state");
    if (prop == 0 || adt_prop_size(prop) != 4) {
        /* 19A348, 20A362: nothing to say. */
        adt_wr32(mark, 0xdd03);
        return;
    }
    if (adt_rd32(prop + ADT_PROP_HEADER) != 0) {
        adt_wr32(prop + ADT_PROP_HEADER, 0);
        say("OpenBootROM: /arm-io/dcp0-expert remote-power-state 1 -> 0"
            " (no dcpexpert-service behind the model)\n");
    }
    adt_wr32(mark, 0xddff);
}

/*
 * The presented display's dot pitch, where it is not the glass's.
 *
 * A d16's panel is 1080x2340 at 476 ppi and the ADT says so
 * (`/arm-io/disp0/dot-pitch` 476); the display this machine presents is the
 * 1125x2436 canvas at 3x, the iPhone X geometry.  libMobileGestalt classes
 * the screen by display size x display-scale x dot pitch, and 1125x2436x3x476
 * is in no row of its table (MGIOMFBSupport, the same in 19A348, 20A362,
 * 23A341 and 24A437), so main-screen-class came back -1 and OnBoardingKit
 * sized every Setup header icon box to 0: the Wi-Fi glyph over its title,
 * the Region globe a fifth of its size (on 20A362).  At 458 the
 * row is 1125x2436x3x458, class 15.  The platform description's
 * `machine.display_pitch` carries the same argument
 * (tests/platform/fixtures/platforms/d16ap.json); the table is generated
 * from it and matched on disp0's own canvas and pitch.
 *
 * Breadcrumb at scratch + 212: 0xd700 entered, 0xd701 Apple's dcpf, 0xd702
 * no disp0, 0xd703 no row matches, 0xd7ff rewritten.
 */
void sptm_handoff_display_pitch(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_display_pitch(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 212);
    uint8_t *disp0;
    uint8_t *w;
    uint8_t *h;
    uint8_t *pitch;
    unsigned i;

    adt_wr32(mark, 0xd700);
    if (rom_real_dcp_firmware()) {
        adt_wr32(mark, 0xd701);
        return;
    }
    disp0 = adt_child(adt_child(root, "arm-io"), "disp0");
    w = disp0 != 0 ? adt_prop(disp0, "canvas-width") : 0;
    h = disp0 != 0 ? adt_prop(disp0, "canvas-height") : 0;
    pitch = disp0 != 0 ? adt_prop(disp0, "dot-pitch") : 0;
    if (w == 0 || h == 0 || pitch == 0 || adt_prop_size(w) != 4
        || adt_prop_size(h) != 4 || adt_prop_size(pitch) != 4) {
        adt_wr32(mark, 0xd702);
        return;
    }
    for (i = 0; i < AVP_DISPLAY_PITCH_COUNT; i++) {
        const AVPDisplayPitch *row = &avp_display_pitch[i];

        if (adt_rd32(w + ADT_PROP_HEADER) == row->canvas_width
            && adt_rd32(h + ADT_PROP_HEADER) == row->canvas_height
            && adt_rd32(pitch + ADT_PROP_HEADER) == row->panel_dot_pitch) {
            adt_wr32(pitch + ADT_PROP_HEADER, row->dot_pitch);
            say("OpenBootROM: /arm-io/disp0 dot-pitch ");
            say_hex(row->panel_dot_pitch);
            say(" -> ");
            say_hex(row->dot_pitch);
            say(" (the presented canvas's)\n");
            adt_wr32(mark, 0xd7ff);
            return;
        }
    }
    adt_wr32(mark, 0xd703);
}

void sptm_handoff_ans_firmware(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_ans_firmware(uint64_t tree, uint64_t scratch)
{
    static const char preloaded[] = "pre-loaded";
    static const char seg_names[] = "segment-names";
    static const char seg_ranges[] = "segment-ranges";
    static const char text_data[] = "__TEXT;__DATA";
    static const char fw_name[] = "firmware-name";
    static const char ans2[] = "ANS2";
    static const char running[] = "running";
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 72);
    uint8_t *nub;
    uint8_t *prop;
    unsigned c;

    adt_wr32(mark, 0xa5e0);

    /* pre-loaded, out of the zero-length shutdown-sleep */
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "ans"), "iop-ans-nub");
    if (nub == 0) {
        adt_wr32(mark, 0xa5e1);
        return;
    }
    prop = adt_prop(nub, "shutdown-sleep");
    if (prop == 0 || adt_resize_prop(root, prop, 4) == 0) {
        adt_wr32(mark, 0xa5e2);
        return;
    }
    adt_rename_prop(prop, preloaded, sizeof(preloaded));
    adt_wr32(prop + ADT_PROP_HEADER, 1);

    /* segment-names, out of the zero-length continuous-time */
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "ans"), "iop-ans-nub");
    prop = adt_prop(nub, "continuous-time");
    if (prop == 0 || adt_resize_prop(root, prop, sizeof(text_data)) == 0) {
        adt_wr32(mark, 0xa5e3);
        return;
    }
    adt_rename_prop(prop, seg_names, sizeof(seg_names));
    for (c = 0; c < sizeof(text_data); c++) {
        prop[ADT_PROP_HEADER + c] = (uint8_t)text_data[c];
    }

    /* segment-ranges, out of region-base -- the same four pairs iBoot writes:
     * each segment gets its physical address with its virtual address, then
     * its physical address again with its size. */
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "ans"), "iop-ans-nub");
    prop = adt_prop(nub, "region-base");
    if (prop == 0 || adt_resize_prop(root, prop, 64) == 0) {
        adt_wr32(mark, 0xa5e4);
        return;
    }
    adt_rename_prop(prop, seg_ranges, sizeof(seg_ranges));
    adt_wr64(prop + ADT_PROP_HEADER + 0,  ANS_FW_BASE);
    adt_wr64(prop + ADT_PROP_HEADER + 8,  0);
    adt_wr64(prop + ADT_PROP_HEADER + 16, ANS_FW_BASE);
    adt_wr64(prop + ADT_PROP_HEADER + 24, (1ULL << 32) | ANS_FW_TEXT_SIZE);
    adt_wr64(prop + ADT_PROP_HEADER + 32, ANS_FW_BASE + ANS_FW_TEXT_SIZE);
    adt_wr64(prop + ADT_PROP_HEADER + 40, ANS_FW_TEXT_SIZE);
    adt_wr64(prop + ADT_PROP_HEADER + 48, ANS_FW_BASE + ANS_FW_TEXT_SIZE);
    adt_wr64(prop + ADT_PROP_HEADER + 56, ANS_FW_DATA_SIZE);

    /* firmware-name, out of region-size, which describes a region that is not
     * there.  iop-gfx-nub carries one in the board's own ADT and it is the key
     * iBoot looks the image up by; RTBuddy wants a name for what it announces.
     * A name is read to its terminator, so eight bytes holding four and four
     * zeroes reads as the four. */
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "ans"), "iop-ans-nub");
    prop = adt_prop(nub, "region-size");
    if (prop == 0) {
        adt_wr32(mark, 0xa5e5);
        return;
    }
    adt_rename_prop(prop, fw_name, sizeof(fw_name));
    for (c = 0; c < 8u; c++) {
        prop[ADT_PROP_HEADER + c] = (uint8_t)(c < sizeof(ans2) ? ans2[c] : 0);
    }

    /*
     * running -- and this is the property the whole description turns on.
     *
     * RTBuddy::_initConfigEDT reads three things off the nub and combines them
     * into one decision: pre-loaded, running, and no-firmware-service.  If
     * none of the last two is there, _attemptFirmwareLoad does not load
     * anything at all; it registers a matching notification and waits for a
     * RTBuddyFirmwareService to publish this processor firmware.  On a restore
     * boot nothing ever publishes ANS firmware, so RTBuddy(ANS2) prints its
     * start line and then waits for good -- which is why the mailbox interrupt
     * was never enabled, why no endpoint was ever announced, and why
     * AppleANS2NVMeController::start sat on ANS2Endpoint1 with no timeout.
     * pre-loaded alone does not reach that code.
     *
     * running is the accurate one of the two, not merely the one that works:
     * this core is already executing when the kernel first looks, because the
     * machine model starts it at reset and its greeting is already in the
     * mailbox.  RTBuddyFirmware::fixup reads the same flag and skips
     * copyToTarget when it is set, which is exactly right -- there is nothing
     * to copy, and the segment-ranges above describe DRAM that holds no image.
     */
    nub = adt_child(adt_child(adt_child(root, "arm-io"), "ans"), "iop-ans-nub");
    if (nub == 0 || adt_add_prop(root, nub, running, sizeof(running), 1) == 0) {
        adt_wr32(mark, 0xa5e6);
        return;
    }

    adt_wr32(mark, 0xa5ff);
}

/*
 * The watchdog, told not to look for a power management unit.
 *
 * restored_external ends its run at checkpoint 0x0405, disable_watchdog, with
 * "Failed to discover watchdog service", and exits 1 -- which is what stops
 * every boot from getting past the first minute of userspace.  What it cannot
 * find is IOWatchdog, and IOWatchdog is published at the very end of
 * AppleARMWatchdogTimer::start, after this:
 *
 *     if (getProperty(provider, "no-pmu") == 0) {
 *         dict = resourceMatching("IOPMU");
 *         svc  = waitForService(dict, NULL);      // NULL: forever
 *         ...
 *     }
 *
 * Measured on the live guest rather than inferred: the object is at
 * qword_FFFFFFF02A33F868, and the field the block fills, this+0x130, is still
 * zero while this+0x12c already holds 0x55d4a800 -- the sixty-second reset
 * count written much earlier in the same start.  So the start is inside that
 * wait, not before it and not after it.
 *
 * Nothing publishes IOPMU.  The resource comes from AppleDialogSPMIPMU::start,
 * which prints "PMU detected" and publishes only if the part is primary;
 * /arm-io/nub-spmi0/aintreeE is marked is-primary and is addressed at SPMI
 * slave 0x0e, and over a whole boot not one transaction is issued to that
 * slave -- every access on nub-spmi0 goes to 0xf.  That driver is a debt to
 * come back to, not something to fake here.
 *
 * What the watchdog wants from the PMU is one flag, info-has_phra, off the
 * PMU's own provider; the property makes it skip the lookup and go on to
 * publish.  It is the supported way to say a watchdog has no PMU beside it,
 * and it is narrower than taking a device out of the tree.
 */
/*
 * The sealed system volume's root hash allowed to differ -- as a device-tree
 * property, which is what the check actually reads.
 *
 * `allow-root-hash-mismatch` is **not** a boot argument.  APFS's
 * `apfs_root_hash_validation_is_disabled` is
 *
 *     DT_property("/options", "allow-root-hash-mismatch", &obj)
 *     -> OSData, length 1, and the byte is not ASCII '0'
 *
 * so appending it to the command line -- which this ROM already does, and the
 * kernel duly prints in "Boot args:" -- changes nothing.  It is an NVRAM
 * variable, and NVRAM variables reach the kernel as properties of the ADT's
 * `options` node.  restored writes it there itself when it skips sealing; this
 * chain has no restored, and iBoot reads the NVRAM partition into a buffer
 * without importing it, so the node arrives empty and the check stays on:
 *
 *     is_root_hash_authentication_required:478: root hash authentication is required
 *     validate_on_disk_root_hash:346: root_hash mismatch, expected 93e0b24d... got 2d9136ef...
 *     panic: "APFSX: could not validate root hash Authentication error (80)" @apfs_vfsops.c:539
 *
 * The seal is self-consistent -- `apfs_sealvolume` built it from the disk
 * because the restore ran with `SealedSystemVolumeAuthenticate` false -- so the
 * hash simply is not the one Apple signed, and this is the switch that says so.
 */
void sptm_handoff_root_hash_mismatch(uint64_t tree, uint64_t scratch);

/*
 * `/filesystems`'s `e-apfs`, for a boot that says it is not one.
 *
 * `usermanagerd` decides Enhanced APFS mode by whether that property exists --
 * `IORegistryEntryCreateCFProperty(IODeviceTree:/filesystems, "e-apfs")`
 * against NULL, no value tested (21A329's own usermanagerd).  In
 * that mode its early boot task looks for a primary user volume, finds
 * disk1s8, and tries to migrate its media key:
 *
 *     apfs_migrate_aks_key_class:2364: failed to get wrapped media key
 *         (uuid = 61706673-7575-6964-0002-766F6C756D07, cur_tag = 8), error = 2
 *     UMD_INIT: APFSVolumeMigrateMediaKey Faled with Error:2, PANIC!!
 *
 * and SIGABRTs, so there is no user session, no SpringBoard and no window
 * server.  The volume this tree's restore makes carries wrapped media keys
 * for tags 2 and 13 and none for tag 8.  Nothing inside usermanagerd gets
 * past that: `um-migration-debug=101` is the build's own documented way out
 * of this exact panic and cannot be reached, because its parser tests
 * `um-migration-debug=1` first and `=101` contains it; and doing by hand what
 * 101 would do -- removing /private/var/keybags/usersession.kb -- only moves
 * the abort along to `Erase Install or normal boot, no USerSessions to
 * Load...`, measured on 21A329.
 *
 * Without the property it logs `Not in Enhanced APFS Mode..` and carries on.
 *
 * The property is iBoot's: it is in neither the board's ADT nor anything this
 * tree writes, so here is where it can be taken off.  Renamed, not removed --
 * the tree is full on 24A437 and a grow is refused silently, and a rename
 * costs nothing.  **Only for a boot that asks**, because 24A437 has a
 * working UI on this chain and is in Enhanced APFS mode while it does:
 * `noeapfs=1` in the boot-args, read out of the blob the machine leaves at
 * BOOT_ARGS_EXTRA, exactly as `scaler0=1` is.
 */
void sptm_handoff_no_eapfs(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_no_eapfs(uint64_t tree, uint64_t scratch)
{
    static const char handoff_eapfs_arg[] = "noeapfs=1";
    static const char inert[] = "x-apfs";
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 156);
    uint8_t *fs;
    uint8_t *prop;
    unsigned found = 0;

    adt_wr32(mark, 0x5f00);
    {
        static const char magic[] = "OMVBARG";
        const volatile char *extra = (const volatile char *)BOOT_ARGS_EXTRA;
        unsigned k;

        for (k = 0; k < sizeof(magic) - 1; k++) {
            if (extra[k] != magic[k]) {
                break;
            }
        }
        if (k == sizeof(magic) - 1) {
            const volatile char *host = extra + sizeof(magic);
            unsigned at;

            for (at = 0; host[at] != '\0' && found == 0; at++) {
                unsigned j = 0;

                while (handoff_eapfs_arg[j] != 0
                       && host[at + j] == handoff_eapfs_arg[j]) {
                    j++;
                }
                found = (handoff_eapfs_arg[j] == 0);
            }
        }
    }
    if (found == 0) {
        adt_wr32(mark, 0x5f01);
        return;
    }
    fs = adt_child(root, "filesystems");
    if (fs == 0) {
        adt_wr32(mark, 0x5f02);
        return;
    }
    prop = adt_prop(fs, "e-apfs");
    if (prop == 0) {
        /* iBoot did not put it there on this build; nothing to do. */
        say("OpenBootROM: /filesystems has no e-apfs\n");
        adt_wr32(mark, 0x5f03);
        return;
    }
    adt_rename_prop(prop, inert, sizeof(inert));
    say("OpenBootROM: /filesystems e-apfs renamed, no Enhanced APFS\n");
    adt_wr32(mark, 0x5f0f);
}

HANDOFF void sptm_handoff_root_hash_mismatch(uint64_t tree, uint64_t scratch)
{
    static const char allow[] = "allow-root-hash-mismatch";
    static const uint8_t yes[] = { '1' };
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 104);
    uint8_t *options;

    adt_wr32(mark, 0xd900);
    options = adt_child(root, "options");
    if (options == 0) {
        adt_wr32(mark, 0xd901);     /* iBoot published no options node */
        return;
    }
    if (adt_prop(options, allow) != 0) {
        adt_wr32(mark, 0xd9ff);     /* already there */
        return;
    }
    if (adt_add_prop_bytes(root, options, allow, sizeof(allow),
                           yes, sizeof(yes)) == 0) {
        adt_wr32(mark, 0xd902);
        return;
    }
    adt_wr32(mark, 0xd9ff);
}

void sptm_handoff_effaceable_storage(uint64_t tree, uint64_t scratch);

/*
 * Take `no-effaceable-storage` out of `/defaults`.
 *
 * `effaceable_is_disabled()` in the kernel is one line --
 * `DT_property("/defaults", "no-effaceable-storage", nullptr)` -- and when it
 * answers, `AppleSEPKeyStore` logs "disabling use of effaceable storage, using
 * fake key, 0xffffffff" and hands sks a key of all ones.  sks then panics with
 * no message from its own key check, which is where the installed 19A348 chain
 * stops now that sars is clear.
 *
 * The property is a zero-length entry -- present, therefore true -- and it is
 * in the *pristine* device tree as well as ours, so it is Apple's for this
 * board.  Removing it is a question, not a fix: does the keystore take the
 * real path and does sks then survive?  Renaming rather than deleting keeps
 * the tree's shape and every offset in it intact, and one byte is enough
 * because `DT_property` matches the whole name.
 */
HANDOFF void sptm_handoff_effaceable_storage(uint64_t tree, uint64_t scratch)
{
    static const char name[] = "no-effaceable-storage";
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 96);
    uint8_t *defaults;
    uint8_t *prop;

    adt_wr32(mark, 0xda00);
    defaults = adt_child(root, "defaults");
    if (defaults == 0) {
        adt_wr32(mark, 0xda01);     /* no defaults node */
        return;
    }
    prop = adt_prop(defaults, name);
    if (prop == 0) {
        adt_wr32(mark, 0xda02);     /* not there to begin with */
        return;
    }
    prop[0] = 'x';                  /* "xo-effaceable-storage" */
    adt_clean(prop, prop + 4);
    adt_wr32(mark, 0xdaff);
}

void sptm_handoff_watchdog_no_pmu(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_watchdog_no_pmu(uint64_t tree, uint64_t scratch)
{
    static const char no_pmu[] = "no-pmu";
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 76);
    uint8_t *wdt;

    adt_wr32(mark, 0xd700);
    wdt = adt_child(adt_child(root, "arm-io"), "wdt");
    if (wdt == 0) {
        adt_wr32(mark, 0xd701);
        return;
    }
    if (adt_prop(wdt, "no-pmu") != 0) {
        adt_wr32(mark, 0xd7ff);
        return;
    }
    if (adt_add_prop(root, wdt, no_pmu, sizeof(no_pmu), 1) == 0) {
        adt_wr32(mark, 0xd702);
        return;
    }
    adt_wr32(mark, 0xd7ff);
}

void sptm_handoff_sep_skip_lynx(uint64_t tree, uint64_t scratch);

/*
 * The Lynx part this board does not have.
 *
 * commit_sep_os is the last step of a restore and its first act is
 * _ramrod_commit_sep_call_verify, which asks MobileGestalt for
 * kMGQLynxSerialNumber -- the obfuscated key "r5pA2qLgR86BQKwgMjPWzg".
 * libMobileGestalt answers it the same way in 19A348 and in 21A329: open a
 * user client on AppleSEPManager and call external method 42 with a 32 KB
 * out-struct (libMG15 0x14198, libMG17 0xd0d8; both tail-call the same
 * IOConnectCallMethod helper, which returns NULL when the method fails).
 * restored reads that answer twice -- a non-zero MGCopyAnswerWithError error
 * is fatal, and a NULL answer with no error is
 *
 *     Device does not have Lynx, continuing..
 *
 * which is the line the verified 19A348 restore prints and goes on from.
 *
 * On 15 method 42 fails here and the restore takes that arm.  On 17 it
 * succeeds, because 21A329's AppleSEPManager::hasLynxFamily looks for a child
 * named "Lynx" or "Ocelot" and this board's own tree carries
 * /arm-io/sep/iop-sep-nub/Ocelot.  So restored believes there is a Lynx part,
 * goes looking for its certificate, finds the FDR lcrt class empty, and ends
 * at
 *
 *     Lynx cert missing or failed trust eval.  Nuking.  TrustError=0x0
 *     No LynxData, calling seputil without passing cert.
 *     panic(...): SEP Panic: :sars/sars: ...
 *
 * 21A329 is also the build that added the switch for exactly this.  Method 42
 * (kernelcache 0xfffffff0291363b8) begins
 *
 *     if (!hasLynxFamily() || this->skipLynx) return kIOReturnBadArgument;
 *
 * and skipLynx is set once in AppleSEPManager::start (0xfffffff029147054) from
 *
 *     getProperty("skip-lynx", gIOServicePlane,
 *                 kIORegistryIterateRecursively | kIORegistryIterateParents)
 *
 * on its provider -- so the property may sit on /arm-io/sep or on any ancestor
 * -- and stored at +0x199.  That byte has exactly one reader in the whole
 * kernelcache, method 42, so this disables the serial-number read and nothing
 * else: LcrtVerify (method 41) and AppleSEPXART::lynx_authenticate are not
 * gated by it.
 *
 * Saying it is a statement about this board, not about a check.  The part
 * behind Ocelot here is the emulator's Lynx crypto model, whose identity key is
 * ours; no Apple certificate for it can exist, so the road method 42 opens has
 * no end but that panic.  This puts 21A329 on the arm 19A348 already takes.
 *
 * 19A348 cannot see the property: neither "skip-lynx" nor hasLynxFamily exists
 * anywhere in that kernelcache, and its AppleSEPManager::start never reads it.
 */
HANDOFF void sptm_handoff_sep_skip_lynx(uint64_t tree, uint64_t scratch)
{
    static const char skip_lynx[] = "skip-lynx";
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 136);
    uint8_t *sep;

    adt_wr32(mark, 0x5f00);
    sep = adt_child(adt_child(root, "arm-io"), "sep");
    if (sep == 0) {
        adt_wr32(mark, 0x5f01);
        return;
    }
    if (adt_prop(sep, "skip-lynx") != 0) {
        adt_wr32(mark, 0x5fff);
        return;
    }
    /*
     * With the tree's real length, not the constant.  Measured 2026-09-28 on
     * 24A437: this step reported nothing and /arm-io/sep carried no
     * skip-lynx, because the tree is already past ADT_TREE_CAPACITY and the
     * add was refused -- so commit_sep_os went looking for a Lynx
     * certificate, `Lynx cert missing or failed trust eval.  Nuking.`, and
     * sars panicked in its own initialiser with xART waiting behind it.
     */
    if (adt_add_prop_cap(root, sep, skip_lynx, sizeof(skip_lynx), 1,
                         adt_rd32((uint8_t *)(scratch + 208))) == 0) {
        adt_wr32(mark, 0x5f02);
        return;
    }
    say("OpenBootROM: /arm-io/sep carries skip-lynx\n");
    adt_wr32(mark, 0x5fff);
}

/*
 * `ane-disabled` on /arm-io/pmgr/clpc, only when the boot asks with
 * `clpcnoane=1`.
 *
 * The boot-arg ` ane-disabled=1` this file appends is CLPC's override of a
 * *property*, and 23A341's AppleT8110CLPC only consults the boot-arg when the
 * property is there: its provider's getProperty("ane-disabled") cast to
 * OSData, then PE_parse_boot_argn("ane-disabled", &v, 4) over the value, and
 * with no property the byte stays 0 and the boot-arg is never read
 * (kc: 23A341 0xfffffff0098bc48c..0x...bc500).  CLPC then builds
 * the ANE's operating points from DVFMStates' fANE0 column, which is all zero
 * here, and reads element -1 of a stack array -- "Kernel data abort ... esr
 * 0x96000006 far 0xfffffff1a072f89c" at 0x...98bf05c, the same abort the
 * boot-arg was added for on 24A437.
 * Opt-in, so no chain that never passed `clpcnoane=1` -- 19A348 among them --
 * sees a new property.
 */
void sptm_handoff_clpc_no_ane(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_clpc_no_ane(uint64_t tree, uint64_t scratch)
{
    static const char arg[] = "clpcnoane=1";
    static const char name[] = "ane-disabled";
    static const char magic[] = "OMVBARG";
    const volatile char *extra = (const volatile char *)BOOT_ARGS_EXTRA;
    uint8_t *root = (uint8_t *)tree;
    uint8_t *clpc;
    unsigned k;
    unsigned found = 0;

    for (k = 0; k < sizeof(magic) - 1; k++) {
        if (extra[k] != magic[k]) {
            return;
        }
    }
    {
        const volatile char *host = extra + sizeof(magic);
        unsigned at;

        for (at = 0; host[at] != '\0' && found == 0; at++) {
            unsigned j = 0;

            while (arg[j] != 0 && host[at + j] == arg[j]) {
                j++;
            }
            found = (arg[j] == 0);
        }
    }
    if (found == 0) {
        return;
    }
    clpc = adt_child(adt_child(adt_child(root, "arm-io"), "pmgr"), "clpc");
    if (clpc == 0 || adt_prop(clpc, name) != 0) {
        return;
    }
    if (adt_add_prop_cap(root, clpc, name, sizeof(name), 1,
                         adt_rd32((uint8_t *)(scratch + 208))) != 0) {
        say("OpenBootROM: clpcnoane=1, /arm-io/pmgr/clpc carries ane-disabled\n");
    }
}

/*
 * Tell the display it has an adaptive panel, so CoreAnimation will build its
 * mode list.
 *
 * `CA::WindowServer::IOMFBDisplay::update_framebuffer_locked`
 * (qc:0x1847b4940) builds a display's mode list out of the timings and colour
 * modes the display controller publishes only when
 *
 *     CADeviceSupportsAPT() || CADeviceIsVirtualized()
 *
 * is true.  Otherwise it adds one synthetic mode, -1, and nothing else -- and
 * `update_framebuffer` powers a display on only when its current mode is one
 * of `available_modes()`.  -1 never is, so the display stays off,
 * `IOMFBDisplay::set_mode` keeps refusing, the window server never tells the
 * controller which mode it chose, and not one frame is ever submitted.
 *
 * Measured on 24A437 without either half:
 *
 *     Display 1 current mode (16383 x 16383) [0 0], \
 *         preferred mode (0 x 0) [0 0]
 *
 * where 0x3fff/0x3fff is -1 read through a Mode's two 14-bit width and height
 * fields -- and `swap_submit` zero across a whole boot.  With it:
 * `set_digital_out_mode: timing 1 colour 1` arrives and swaps are submitted.
 *
 * **Which half matters, and this is the whole point of this step.**  The
 * other one, `CADeviceIsVirtualized`, is `MGGetBoolAnswer("IsVirtualDevice")`
 * -> `sysctlbyname("kern.hv_vmm_present")` -> `IODTGetDefault("vmm-present")`,
 * and answering it true does open the gate -- but the same answer is read
 * again, much later, by `IOSurfaceAcceleratorCreate` in
 * /System/Library/PrivateFrameworks/IOSurfaceAccelerator.framework (which
 * IOSurface dlopens on first use).  On a device that calls itself virtual
 * that function matches **`AppleM2ScalerParavirtDriver`** instead of
 * `AppleM2ScalerCSCDriver`, logs `IOServiceMatchingfailed for:
 * AppleM2ScalerParavirtDriver`, and returns 0xE0000000.  No kext of that
 * class exists anywhere in this kernelcache, and CoreAnimation does not
 * degrade on the failure: `CA::IOSAManager::create_new_iosa_manager` calls
 * **abort()**.  Measured -- backboardd's own hang report names it:
 *
 *     abort +148
 *     CA::IOSAManager::create_new_iosa_manager(__CFRunLoop*) +572
 *     CA::WindowServer::Display::copy_iosa_manager() +56
 *     CA::WindowServer::IOSurface::copy_iosurface(...) +268
 *     CA::WindowServer::IOMFBDisplay::copy_surface(...) +1040
 *
 * so the display thread dies on the first vsync that needs a copy, backboardd
 * goes with it, and launchd reboots on SpringBoard.
 *
 * APT has no such second reader.  `CADeviceSupportsAPT` is
 * `MGGetBoolAnswer("SupportsPerseus")`, which libMobileGestalt answers from
 * `/arm-io/disp0`'s `supports-apt` -- a four-byte property, nonzero.  d16's
 * tree has no such property, so one is made out of `mipi-support-ioav`, the
 * node's only other four-byte property whose value is already 1.  A rename
 * keeps the record's size, so nothing after it moves and the tree does not
 * have to grow -- which on 24A437 it cannot, being already past
 * ADT_TREE_CAPACITY, where every grow is refused silently.
 */
void sptm_handoff_supports_apt(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_supports_apt(uint64_t tree, uint64_t scratch)
{
    static const char want[] = "supports-apt";
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 152);
    uint8_t *disp0;
    uint8_t *prop;

    adt_wr32(mark, 0xa5d1);
    disp0 = adt_child(adt_child(root, "arm-io"), "disp0");
    if (disp0 == 0) {
        adt_wr32(mark, 0xa5d2);
        return;
    }
    if (adt_prop(disp0, want) != 0) {
        /* The board already says it; leave the donor alone. */
        adt_wr32(mark, 0xa5df);
        return;
    }
    prop = adt_prop(disp0, "mipi-support-ioav");
    if (prop == 0 || adt_prop_size(prop) != 4) {
        adt_wr32(mark, 0xa5d3);
        return;
    }
    adt_rename_prop(prop, want, sizeof(want));
    adt_wr32(prop + ADT_PROP_HEADER, 1);
    say("OpenBootROM: /arm-io/disp0 carries supports-apt\n");
    adt_wr32(mark, 0xa5df);
}

/*
 * The board this machine says it is.
 *
 * restored's fdr_create step asks MobileGestalt for HWModelStr.  That is
 * sysctl hw.target, which XNU answers out of PEGetTargetName; on arm64 that
 * is IODTPlatformExpert::getModelName, and it returns the first string of the
 * device tree root's `compatible`.  restored treats a failure in FDR as fatal
 * unless that string ends in "DEV" -- ramrod_device_hide_fdr_failure -- which
 * is how Apple's own development boards get through a restore with no factory
 * data behind them.  This machine has none either: there is no FDR server to
 * seal against and the ticket carries no rfta or ftap digest, so RestoredFDRCreate
 * cannot get past reading them and every restore ends at step 0x0616.
 *
 * Saying "D16DEV" is what the hardware actually is, not a way around a check.
 * The old and the new string round up to the same four-byte property slot, so
 * the record keeps its size and nothing after it moves.
 */
void sptm_handoff_dev_board(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_dev_board(uint64_t tree, uint64_t scratch)
{
    static const char want[] = "D16DEV";
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 80);
    uint8_t *prop;
    uint8_t *data;
    uint32_t raw;
    uint32_t size;
    uint32_t slot;
    uint32_t first;
    uint32_t oldlen;
    uint32_t newlen;
    uint32_t tail;
    uint32_t j;

    adt_wr32(mark, 0xdb00);
    prop = adt_prop(root, "compatible");
    if (prop == 0) {
        adt_wr32(mark, 0xdb01);
        return;
    }
    raw = adt_rd32(prop + ADT_NAME_LEN);
    size = raw & 0x7fffffffu;
    slot = (size + 3u) & ~3u;
    data = prop + ADT_PROP_HEADER;

    first = 0;
    while (first < size && data[first] != 0) {
        first++;
    }
    if (first >= size) {                    /* not NUL-terminated; leave it be */
        adt_wr32(mark, 0xdb02);
        return;
    }
    if (first >= 3 && data[first - 3] == 'D' && data[first - 2] == 'E' &&
        data[first - 1] == 'V') {
        adt_wr32(mark, 0xdbff);             /* already a dev board */
        return;
    }

    oldlen = first + 1;                     /* the string and its NUL */
    newlen = (uint32_t)sizeof(want);        /* "D16DEV" and its NUL */
    tail = size - oldlen;
    if (newlen + tail > slot) {
        adt_wr32(mark, 0xdb03);
        return;
    }
    if (newlen > oldlen) {
        for (j = tail; j > 0; j--) {
            data[newlen + j - 1] = data[oldlen + j - 1];
        }
    } else {
        for (j = 0; j < tail; j++) {
            data[newlen + j] = data[oldlen + j];
        }
    }
    for (j = 0; j < newlen; j++) {
        data[j] = (uint8_t)want[j];
    }
    adt_wr32(prop + ADT_NAME_LEN, (raw & 0x80000000u) | (newlen + tail));
    adt_clean(prop, prop + ADT_PROP_HEADER + slot);
    adt_wr32(mark, 0xdbfe);
}

void sptm_handoff_ignore_devices(uint64_t tree, uint64_t scratch);
void sptm_handoff_serial_dock_channel(uint64_t tree, uint64_t scratch);

/*
 * The ones that hang off another block rather than off arm-io.
 *
 * tristar is the accessory identification chip on the SMC's own i2c bus --
 * `tristar,cbtl1614`, a child of smc-i2c1 rather than of arm-io, so neither
 * list above reaches it.  AppleKraken talks to it over an IPC this machine
 * does not answer and stops the boot rather than failing:
 *
 *     AppleKraken: could not find system ID: bad IPC message ID 0 0 0 0 d 0
 *     @IOAccessoryManagerAID.cpp:44
 *
 * What it identifies is what is plugged into the port.  A restore arriving
 * over USB does not need that answered -- the host is driving the transfer --
 * so the chip is taken out of the tree rather than modelled.
 */
static const struct {
    const char *parent;          /* a child of /arm-io            */
    const char *middle;          /* one more level down, or 0     */
    const char *child;
} handoff_ignored_under[] = {
    /*
     * The high-voltage charger, whose probe failure kills the kernel.
     *
     * `AppleSMCHighVoltageCharger::_readDeviceInfo` waits for `portmicro` --
     * the port microcontroller, `portmicro,smc-ccg2` in the tree -- to come
     * out of reset.  Nothing here answers for it, so the probe gives up with
     * `probe: failed (84) 0x84 (UNDEFINED)`, and the path it takes on the way
     * out frees a 224-byte allocation against the 32-byte zone:
     *
     *     not in the expected zone shared.kalloc.32[63],
     *     but found in shared.kalloc.224[215] @zalloc.c:791
     *
     * Reproducible, same zone pair every time, and the backtrace is
     * IOConfigThread::main -> doServiceMatch -> probeCandidates -> the
     * AppleSMC kext.  So the panic is not corruption; it is a failure path
     * that only a machine without a portmicro ever walks.
     *
     * Taking the node out means the driver never matches and the path is never
     * entered.  **`portmicro` itself is still owed** -- charging, accessory
     * identification and USB-C power delivery all live behind it.
     */
    { "smc", "iop-smc-nub", "smc-ext-charger" },
    /*
     * And portmicro itself, which is what actually keeps the panic away.
     *
     * Masking smc-ext-charger alone does nothing: AppleSMC walks its own
     * children rather than going through `IODTFindMatchingEntries`, so it
     * never consults `AAPL,ignore` -- measured, the property was in the live
     * tree and `AppleSMCHighVoltageCharger` probed anyway.
     *
     * The destructor is what panics, and it is conditional:
     *
     *     if (*(this + 488) != 0)
     *         kfree_type(&unk_FFFFFFF027B016A0);   // the wrong view
     *     IOService::free(this);
     *
     * `this + 488` is the portmicro provider -- the same object
     * `_readDeviceInfo` asks to come out of reset (vtable +88) and for its
     * device info (vtable +64).  With no portmicro node there is no provider,
     * the field stays 0, and the mis-sized free never runs.  The probe still
     * fails; it just fails without taking the kernel with it.
     *
     * This node does carry `compatible`, so the mask lands.
     */
    { "smc", "iop-smc-nub", "portmicro" },
    /*
     * tristar is back.
     *
     * It was taken out because a restore arriving over USB does not need to be
     * told what is plugged in -- the host drives the transfer.  The installed
     * chain does: nothing else answers "there is a USB host on the port", so
     * `IOUSBDeviceController` builds every configuration it has (AppleUSBMux,
     * AppleUSBEthernet, NCM) and then never connects the gadget.  Measured on
     * a live guest: the dwc3's DCFG, DCTL and DEVTEN are all zero, so
     * `apple-dwc3.c` never reaches `usb_device_attach` and the USB/IP server
     * keeps exporting the ROM's own recovery device.  The registry has one
     * port, `Port-Inductive` -- the wireless one -- and no Lightning port at
     * all, because the chip that identifies it is not in the tree.
     *
     * The part itself is modelled (`tristar_answers` in apple-iphone.c), with
     * the two registers iBoot's payload asked for.  AppleCBTL1610 will ask for
     * more; a part that answers zero keeps a driver asking, and the log names
     * the next question.
     */
    /* { "smc-i2c1", 0, "tristar" }, */
    /*
     * The NFC controller, which a restore refuses to finish without.
     *
     * `restored`'s async checkpoint **update_stockholm** asks
     * `_NfRestoreHasRadio`, is told "Radio present." because the tree carries
     * these two nodes, and then tries to bring the part up over SPMI:
     *
     *     AppleStockholmControl::toggleEnable:531 failed to de-assert enable !
     *     nfrestore: _NFHardwareGPIOSetValue selector=13 : Failed with 0x...
     *     nfrestore: phLibNfc_Mgt_Initialize: NFCC Power On Failed!!
     *     nfrestore: NfRestoreReturnError:2532 Failed to shut down Stockholm
     *     Update[Stockholm] error ... "NfRestore Failed. NfResult: 5"
     *     [AsyncCP][End] Checkpoint update_stockholm finished with result: -1.
     *
     * One failed checkpoint is the whole restore: restored sends the host a
     * status message of -1, the host driver raises "Restore failed with status
     * -1" and closes the connection, and every later progress message on the
     * guest gets EPIPE.  Measured at 44 %, with ASR
     * already 675 MB into the image.
     *
     * `update_baseband` and `update_vinyl` in the same chain answer "updater
     * not supported" and pass, because nothing claims that hardware exists.
     * So the fix is the same shape: with no node there is no radio, the
     * updater has nothing to update, and the checkpoint passes.  Both carry
     * `compatible` (`nfc,primary,spmi` and `nfc,secondary,spmi`), so the mask
     * lands.  **The part itself is still owed** -- a real Stockholm would be a
     * SPMI slave with a firmware download protocol of its own.
     */
    { "aop-spmi0", 0, "stockholm-spmi" },
    { "aop-spmi0", 0, "hammerfest-spmi" },
};

/*
 * Let the USB device side come up, so the host can reach a port in the guest.
 *
 * `AppleUSBXDCIARM::start` builds "<match category>-disable" -- for this
 * driver, "usb-device-disable" -- and asks a registry entry for a property of
 * that name.  When one answers it logs
 *
 *     AppleT8027USBXDCI@: AppleUSBXDCIARM::start: usb-device matching has
 *     been disabled
 *
 * and never brings the gadget up.  usbmuxd then has nothing to multiplex over,
 * and a host-side `usbmux_forward` gets "USB/IP transfer timed out" from a
 * device that is electrically there and answering nothing.  That is the whole
 * reason this chain has no way in except a reboot.
 *
 * Renaming the property is enough, because `getProperty` matches the whole
 * name -- the same trick `sptm_handoff_ignore_devices` plays on `compatible`.
 *
 * **The tree is walked, not indexed.**  The property is not in the device tree
 * the board ships as its standalone ADT (that file has no `-disable`
 * property at all): the installed chain boots the `dtre` image out of the firmware
 * namespace, which is a different file that cannot be read from the host.  So
 * the ROM looks for it where the kernel will, in the tree it is about to be
 * handed, and says how many it found.
 */
HANDOFF static unsigned adt_scrub_prop(
    uint8_t *node,
    const char *want,
    uint8_t to
)
{
    uint32_t nprops = adt_rd32(node) & 0x7fffffffu;
    uint32_t children = adt_rd32(node + 4);
    uint8_t *p = node + 8;
    unsigned renamed = 0;
    uint32_t i;

    for (i = 0; i < nprops; i++) {
        uint32_t size = adt_rd32(p + 32) & 0x7fffffffu;
        unsigned k = 0;

        while (want[k] != '\0' && p[k] == (uint8_t)want[k]) {
            k++;
        }
        if (want[k] == '\0' && p[k] == 0) {
            p[0] = to;
            adt_clean(p, p + 32);
            renamed++;
        }
        p += ADT_PROP_HEADER + ((size + 3u) & ~3u);
    }
    for (i = 0; i < children; i++) {
        renamed += adt_scrub_prop(p, want, to);
        p = adt_node_end(p);
    }
    return renamed;
}

void sptm_handoff_usb_device_enable(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_usb_device_enable(uint64_t tree, uint64_t scratch)
{
    uint8_t *mark = (uint8_t *)(scratch + 44);
    unsigned renamed;

    adt_wr32(mark, 0xd800);
    renamed = adt_scrub_prop((uint8_t *)tree, "usb-device-disable", 'x');
    adt_wr32(mark, 0xd8f0u | (renamed & 0xffu));
    say("OpenBootROM: usb-device-disable renamed ");
    say_hex(renamed);
    say("\n");
}

/*
 * Say what is actually under a bus, because the host has no copy of the tree.
 *
 * The installed chain boots the `dtre` image out of the ANS firmware
 * namespace; the board's standalone ADT is a different file, and the two have
 * already disagreed once -- `usb-device-disable` is in the one the kernel gets
 * and in neither of the ones on disk.  So when a node is expected and its
 * driver never appears, the only way to see whether the node is there is to
 * look here, in the tree about to be handed over.
 *
 * Diagnostic, not a fix: it prints and changes nothing.
 */
HANDOFF static void say_node_name(uint8_t *node)
{
    uint8_t *prop = adt_prop(node, "name");
    uint32_t size;
    uint32_t i;

    if (prop == 0) {
        say("<unnamed>");
        return;
    }
    size = adt_prop_size(prop);
    for (i = 0; i < size; i++) {
        char one[2];

        if (prop[ADT_PROP_HEADER + i] == 0) {
            break;
        }
        one[0] = (char)prop[ADT_PROP_HEADER + i];
        one[1] = '\0';
        say(one);
    }
}

/*
 * Every property of a node, name and value, for the two nodes that decide
 * whether a port gets a driver.
 *
 * `IOPort`'s only personality is IONameMatch ["port"] against IOPlatformDevice,
 * and IOKit compares that exactly against name/compatible/device_type/model.
 * Neither port node in `build/dtre.bin` has any of those equal to "port", yet
 * port-inductive-1 gets an IOPort and port-lightning-1 does not -- so the tree
 * on disk is not the tree the kernel is handed, and the difference has to be
 * read here rather than off a file.
 */
HANDOFF static void say_node_props(uint8_t *node, const char *label)
{
    uint32_t nprops;
    uint8_t *p;
    uint32_t i;

    say("OpenBootROM: props of ");
    say(label);
    if (node == 0) {
        say(" ABSENT\n");
        return;
    }
    say("\n");
    nprops = adt_rd32(node) & 0x7fffffffu;
    p = node + 8;
    for (i = 0; i < nprops; i++) {
        uint32_t size = adt_prop_size(p);
        uint32_t k;
        int printable = size > 0 && size <= 48;

        say("  ");
        for (k = 0; k < 32 && p[k] != 0; k++) {
            char one[2];
            one[0] = (char)p[k];
            one[1] = '\0';
            say(one);
        }
        say(" = ");
        if (size == 0) {
            say("<empty>");
        } else {
            for (k = 0; printable && k < size; k++) {
                uint8_t c = p[ADT_PROP_HEADER + k];
                if (c == 0 && k + 1 == size) {
                    continue;
                }
                if (c < 0x20 || c > 0x7e) {
                    printable = 0;
                }
            }
            if (printable) {
                say("\"");
                for (k = 0; k < size && p[ADT_PROP_HEADER + k] != 0; k++) {
                    char one[2];
                    one[0] = (char)p[ADT_PROP_HEADER + k];
                    one[1] = '\0';
                    say(one);
                }
                say("\"");
            } else {
                uint32_t w;

                /* Every word of it; see say_words. */
                for (w = 0; w * 4 < size && w < 4u; w++) {
                    uint32_t word = 0;

                    for (k = 0; k < 4 && w * 4 + k < size; k++) {
                        word |= (uint32_t)p[ADT_PROP_HEADER + w * 4 + k]
                                << (k * 8);
                    }
                    if (w != 0) {
                        say(" ");
                    }
                    say_hex(word);
                }
                say(" (");
                say_hex(size);
                say(" bytes)");
            }
        }
        say("\n");
        p = adt_prop_next(p);
    }
}

void sptm_handoff_say_bus(uint64_t tree);

HANDOFF void sptm_handoff_say_bus(uint64_t tree)
{
    static const char *const buses[] = { "smc-i2c1", "i2c2", "i2c3" };
    uint8_t *root = (uint8_t *)tree;
    uint8_t *arm_io = adt_child(root, "arm-io");
    unsigned b;

    if (arm_io == 0) {
        say("OpenBootROM: no arm-io\n");
        return;
    }
    {
        uint32_t children = adt_rd32(root + 4);
        uint8_t *p = adt_after_props(root);
        uint32_t i;

        say("OpenBootROM: / children ");
        say_hex(children);
        say(":");
        for (i = 0; i < children; i++) {
            say(" ");
            say_node_name(p);
            p = adt_node_end(p);
        }
        say("\n");
    }
    say_node_props(adt_child(root, "port-lightning-1"), "port-lightning-1");
    say_node_props(adt_child(root, "port-inductive-1"), "port-inductive-1");
    for (b = 0; b < sizeof(buses) / sizeof(buses[0]); b++) {
        uint8_t *bus = adt_child(arm_io, buses[b]);
        uint32_t children;
        uint8_t *p;
        uint32_t i;

        say("OpenBootROM: ");
        say(buses[b]);
        if (bus == 0) {
            say(" ABSENT\n");
            continue;
        }
        children = adt_rd32(bus + 4);
        say(" children ");
        say_hex(children);
        say(":");
        p = adt_after_props(bus);
        for (i = 0; i < children; i++) {
            say(" ");
            say_node_name(p);
            p = adt_node_end(p);
        }
        say("\n");
    }
}

/*
 * Rename one of a node's properties to `AAPL,ignore`.
 *
 * `IODTFindMatchingEntries` skips a node that carries a property of that name,
 * whatever its value, so masking a node is a rename and nothing more.
 * `compatible` is the obvious victim and the one every earlier caller used --
 * but a leaf like /arm-io/smc/iop-smc-nub/smc-ext-charger does not have one,
 * and silently skipping it is how a mask that looks applied does nothing.
 * `device_type` is the fallback: every node in this tree that matters carries
 * one.
 */
HANDOFF static int adt_mask_node(uint8_t *node)
{
    static const char marker[] = "AAPL,ignore";
    uint8_t *prop = node != 0 ? adt_prop(node, "compatible") : 0;
    unsigned c;

    if (prop == 0 && node != 0) {
        prop = adt_prop(node, "device_type");
    }
    if (prop == 0) {
        return 0;
    }
    for (c = 0; c < 32u; c++) {          /* the name field, ahead of the size */
        prop[c] = (uint8_t)(c < sizeof(marker) ? marker[c] : 0);
    }
    return 1;
}

/*
 * A property the driver must not find, rather than a node to hide.
 *
 * `AppleBTM::start` looks `battery-data-collection` up on its own node and
 * creates `AppleBTMBatteryDataAgent` only when the lookup succeeds (kext
 * 0xfffffff009039c20, `cbz w0` at 0x...c30 in 24A437).  The agent then counts
 * the battery packs and requires at least one:
 *
 *     "AppleBTM: void AppleBTMBatteryDataAgent::findPackEntries():714 "
 *     "REQUIRE failed: packCount > 0 && packCount <= MAX_NUM_BATTERIES"
 *     @AppleBTMBatteryDataAgent.cpp:714
 *
 * -- five seconds into launchd on 24A437, the first wall
 * past CLPC.  The packs come from a smart battery this machine does not have
 * and a restore has no use for.
 *
 * **The value is a provider id, not a flag.**  Setting it to zero left the
 * lookup succeeding and the agent started anyway, one function earlier:
 * "btm: initWithParent batteryDataProvider:0" then
 * "Unsupported battery data provider: 0" @AppleBTMBatteryDataAgent.cpp:239
 * So the property has to be gone, which here means
 * renamed -- the same trick `adt_mask_node` plays, one level down.
 *
 * Masking the node instead is not the same thing: `AppleBTM::start` reads
 * `btm-pmu-type` off it two instructions later and needs that one.
 */
static const struct {
    const char *parent;          /* a child of /arm-io            */
    const char *middle;          /* one more level down, or 0     */
    const char *child;
    const char *prop;            /* renamed, so no lookup finds it */
    const char *to;
} handoff_renamed[] = {
    { "nub-spmi0", 0, "btm", "battery-data-collection",
      "x-battery-data-collection" },
};

/*
 * Point the kernel's platform serial at the dock channel, so touch has a
 * transport.
 *
 * XNU registers exactly one platform serial driver.  `pe_serial.c` reads
 * `/defaults/serial-device` as a phandle, finds that node, reads its
 * `compatible` and matches it against `uart-1,samsung` and
 * `aapl,dock-channels`.  Stock names `/arm-io/uart0` (phandle 0x80 on
 * 24A437), so only the UART kind registers -- measured on the live kernel,
 * the registry at kc:0xFFFFFFF00ADB7BE8 holds exactly one entry whose kind
 * word (`+68`) is 1 and whose `next` is null.
 *
 * `AppleDockChannel::start` asks that layer for kind **2**
 * (kc:0xFFFFFFF00926A2F4 passes its own type word straight to the lookup at
 * kc:0xFFFFFFF00A9F3D40), finds nothing and gives up:
 *
 *     AppleDockChannel::start: channel-name: channel0, fifo-size: 2048bytes
 *     [dockchannel] Failed to start: platform serial driver does not support
 *     interrupts for this device.
 *
 * -- and 2048 is `/arm-io/dockchannel-mtp`'s own `fifo-size`, so the channel
 * that dies is the touch one.  After that nothing drains the channel: the
 * model announces multi-touch and says `drained 0` for ever, which is exactly
 * what a boot shows.
 *
 * So hand the phandle of `/arm-io/dockchannel-uart` -- the node whose
 * `compatible` is `aapl,dock-channels` -- to `/defaults/serial-device`.  The
 * phandle is read out of the tree rather than written here as 0x75, because it
 * is a per-build number.
 *
 * The console goes with it, and that costs nothing: the dock channel is
 * the emulator's second serial port, and the host can log it next to the
 * other consoles.
 */
HANDOFF void sptm_handoff_serial_dock_channel(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 72);
    uint8_t *arm_io;
    uint8_t *dock;
    uint8_t *defaults;
    uint8_t *phandle;
    uint8_t *selector;

    adt_wr32(mark, 0x5e00);
    arm_io = adt_child(root, "arm-io");
    if (arm_io == 0) {
        adt_wr32(mark, 0x5e01);
        return;
    }
    dock = adt_child(arm_io, "dockchannel-uart");
    if (dock == 0) {
        adt_wr32(mark, 0x5e02);
        return;
    }
    phandle = adt_prop(dock, "AAPL,phandle");
    if (phandle == 0 || adt_prop_size(phandle) != 4u) {
        adt_wr32(mark, 0x5e03);
        return;
    }
    defaults = adt_child(root, "defaults");
    if (defaults == 0) {
        adt_wr32(mark, 0x5e04);
        return;
    }
    selector = adt_prop(defaults, "serial-device");
    if (selector == 0 || adt_prop_size(selector) != 4u) {
        adt_wr32(mark, 0x5e05);
        return;
    }
    adt_wr32(selector + ADT_PROP_HEADER, adt_rd32(phandle + ADT_PROP_HEADER));
    adt_wr32(mark, 0x5e10 | (adt_rd32(phandle + ADT_PROP_HEADER) & 0xffu));
}

HANDOFF void sptm_handoff_ignore_devices(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 68);
    uint8_t *arm_io;
    unsigned done = 0;
    unsigned i;

    adt_wr32(mark, 0x5d00);
    arm_io = adt_child(root, "arm-io");
    if (arm_io == 0) {
        adt_wr32(mark, 0x5d01);
        return;
    }
    /*
     * `scaler0`, unless this boot says it has one.
     *
     * AppleM2ScalerCSCDriver matches `arm-io/scaler0` because the tree names
     * it, starts, and dies inside its own start on a null -- the pointer at
     * +0x50 is its hardware layer, and on a machine given no `scaler-version`
     * it was never built:
     *
     *     panic(cpu 0): Kernel data abort ... far 0x0000000000000000
     *     Kernel Extensions in backtrace: AppleM2ScalerCSCDriver
     *
     * This lived in `handoff_ignored[]` until 24A437 needed the node left
     * alone, and taking it off that shared list cost 21A329 its boot: five
     * identical panics at one pc on a chain clean for the thirteen boots
     * before.
     *
     * The device tree does not separate the builds -- 21A329 has
     * `/arm-io/dcp0-expert/remote-power-state` as well, and `scaler0`'s own
     * `perf-workloads` did not do it either -- so the boot says so instead.
     * A host that gives the machine a scaler (`scaler-version=`) puts
     * `scaler0=1` in the boot-args (see BOOT_ARGS_EXTRA); everything else keeps the mask it has always had.
     */
    {
        static const char magic[] = "OMVBARG";
        const volatile char *extra = (const volatile char *)BOOT_ARGS_EXTRA;
        unsigned k;
        unsigned found = 0;

        for (k = 0; k < sizeof(magic) - 1; k++) {
            if (extra[k] != magic[k]) {
                break;
            }
        }
        if (k == sizeof(magic) - 1) {
            const volatile char *host = extra + sizeof(magic);
            unsigned at;

            for (at = 0; host[at] != '\0' && found == 0; at++) {
                unsigned j = 0;

                while (handoff_scaler_arg[j] != 0
                       && host[at + j] == handoff_scaler_arg[j]) {
                    j++;
                }
                found = (handoff_scaler_arg[j] == 0);
            }
        }
        if (found == 0) {
            done += (unsigned)adt_mask_node(adt_child(arm_io, "scaler0"));
        }
    }
    /*
     * `jpeg0`, the same way: AppleJPEGDriver matches the node, and with no
     * block behind it AppleEJPEGHal reads a zero version and builds nothing.
     * Left in the tree only when the boot says it has one.
     */
    {
        static const char magic[] = "OMVBARG";
        const volatile char *extra = (const volatile char *)BOOT_ARGS_EXTRA;
        unsigned k;
        unsigned found = 0;

        for (k = 0; k < sizeof(magic) - 1; k++) {
            if (extra[k] != magic[k]) {
                break;
            }
        }
        if (k == sizeof(magic) - 1) {
            const volatile char *host = extra + sizeof(magic);
            unsigned at;

            for (at = 0; host[at] != '\0' && found == 0; at++) {
                unsigned j = 0;

                while (handoff_jpeg_arg[j] != 0
                       && host[at + j] == handoff_jpeg_arg[j]) {
                    j++;
                }
                found = (handoff_jpeg_arg[j] == 0);
            }
        }
        if (found == 0) {
            done += (unsigned)adt_mask_node(adt_child(arm_io, "jpeg0"));
        }
    }
    /* `dcp`, only when the boot asks with `nodcp=1` (see handoff_nodcp_arg). */
    {
        static const char magic[] = "OMVBARG";
        const volatile char *extra = (const volatile char *)BOOT_ARGS_EXTRA;
        unsigned k;
        unsigned found = 0;

        for (k = 0; k < sizeof(magic) - 1; k++) {
            if (extra[k] != magic[k]) {
                break;
            }
        }
        if (k == sizeof(magic) - 1) {
            const volatile char *host = extra + sizeof(magic);
            unsigned at;

            for (at = 0; host[at] != '\0' && found == 0; at++) {
                unsigned j = 0;

                while (handoff_nodcp_arg[j] != 0
                       && host[at + j] == handoff_nodcp_arg[j]) {
                    j++;
                }
                found = (handoff_nodcp_arg[j] == 0);
            }
        }
        if (found != 0) {
            done += (unsigned)adt_mask_node(adt_child(arm_io, "dcp"));
            say("OpenBootROM: nodcp=1, /arm-io/dcp masked\n");
        }
    }
    /*
     * `/baseband` renamed on every boot that runs from its own volume (see
     * the note above handoff_ignored); a boot iBoot handed a RAMDisk keeps it.
     */
    {
        uint64_t rd_at = 0;
        uint64_t rd_len = 0;
        uint8_t *bb = adt_child(root, "baseband");
        uint8_t *name = bb != 0 ? adt_prop(bb, "name") : 0;

        if (map_range(root, "RAMDisk", &rd_at, &rd_len) == 0 && name != 0
            && adt_prop_size(name) > 0) {
            name[ADT_PROP_HEADER] = (uint8_t)'x';   /* "xaseband" */
            done++;
            say("OpenBootROM: /baseband renamed\n");
        }
    }
    {
        for (i = 0; i < sizeof(handoff_ignored) / sizeof(handoff_ignored[0]);
             i++) {
            uint8_t *node;
            const char *nm = handoff_ignored[i];

            node = adt_child(arm_io, nm);
            if (node == 0) {
                continue;
            }
            done += (unsigned)adt_mask_node(node);
        }
    }
    for (i = 0; i < sizeof(handoff_ignored_root) / sizeof(handoff_ignored_root[0]);
         i++) {
        uint8_t *node = adt_child(root, handoff_ignored_root[i]);

        if (node == 0) {
            continue;
        }
        done += (unsigned)adt_mask_node(node);
    }
    for (i = 0;
         i < sizeof(handoff_ignored_under) / sizeof(handoff_ignored_under[0]);
         i++) {
        uint8_t *parent = adt_child(arm_io, handoff_ignored_under[i].parent);
        uint8_t *node;

        if (parent != 0 && handoff_ignored_under[i].middle != 0) {
            parent = adt_child(parent, handoff_ignored_under[i].middle);
        }
        node = parent != 0
            ? adt_child(parent, handoff_ignored_under[i].child) : 0;
        done += (unsigned)adt_mask_node(node);
    }
    for (i = 0; i < sizeof(handoff_renamed) / sizeof(handoff_renamed[0]); i++) {
        uint8_t *parent = adt_child(arm_io, handoff_renamed[i].parent);
        uint8_t *node;
        uint8_t *prop;
        unsigned c;

        if (parent != 0 && handoff_renamed[i].middle != 0) {
            parent = adt_child(parent, handoff_renamed[i].middle);
        }
        node = parent != 0 ? adt_child(parent, handoff_renamed[i].child) : 0;
        prop = node != 0 ? adt_prop(node, handoff_renamed[i].prop) : 0;
        if (prop == 0) {
            continue;
        }
        for (c = 0; c < 32u; c++) {       /* the name field, ahead of the size */
            const char *to = handoff_renamed[i].to;
            unsigned n;

            for (n = 0; to[n] != 0; n++) {
            }
            prop[c] = (uint8_t)(c < n ? to[c] : 0);
        }
    }
    adt_wr32(mark, 0x5d10 | done);
}

/*
 * Put the payload's own USB controller down before leaving.
 *
 * There is one USB2 controller on the real part and iBoot leaves it quiet
 * before handing over, because the kernel inherits the same block.  This
 * machine has two: the dwc2 at 0x2_6110_0000 that the payload talks to, and
 * the DWC3 at 0x2_6101_0000 that XNU drives.  Nothing hands the first one
 * over, so it kept its pending status with GAHBCFG.GLBL_INTR_EN still set and
 * went on driving AIC line 776 -- a number no node in the kernel's device
 * tree claims, so nothing ever masked or serviced it.  The controller
 * re-presents a level after every unmask, and cpu0 spent the rest of the boot
 * in `_sleh_irq` and `AppleInterruptControllerV2::_readReg32`: measured at
 * 11663 deliveries in one boot, peaking at 7416 a second, against a few
 * hundred for everything else put together, and it is why a phase that should
 * pass in a moment took twenty minutes.
 *
 * Clearing the global interrupt enable is what the model gates on
 * (`gintsts & gintmsk && gahbcfg & GLBL_INTR_EN`), so this is the same thing
 * firmware does: stop the device before giving the machine away.
 */
#define PAYLOAD_USB2_BASE   0x261100000ull
#define DWC2_GAHBCFG        0x008
#define DWC2_GINTMSK        0x018

void sptm_handoff_quiet_payload_usb(uint64_t scratch);

HANDOFF void sptm_handoff_quiet_payload_usb(uint64_t scratch)
{
    volatile uint32_t *const gahbcfg =
        (volatile uint32_t *)(PAYLOAD_USB2_BASE + DWC2_GAHBCFG);
    volatile uint32_t *const gintmsk =
        (volatile uint32_t *)(PAYLOAD_USB2_BASE + DWC2_GINTMSK);
    uint8_t *mark = (uint8_t *)scratch + 108;

    *gintmsk = 0;
    *gahbcfg = 0;
    adt_wr32(mark, 0x5b00);
}

void sptm_handoff_dcs_states(uint64_t tree, uint64_t scratch);

HANDOFF void sptm_handoff_dcs_states(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 60);
    uint8_t *chosen;
    uint8_t *arm_io;
    uint8_t *pool;
    uint8_t *pmgr;
    uint8_t *states;
    uint8_t *donor = 0;
    uint8_t *prop;
    uint8_t *from;
    uint8_t *to;
    uint8_t *end;
    uint32_t count;
    uint32_t raw;
    uint32_t freq;
    uint32_t volt;
    uint32_t i;

    adt_wr32(mark, 0xdc50);
    chosen = adt_child(root, "chosen");
    arm_io = adt_child(root, "arm-io");
    if (chosen == 0 || arm_io == 0) {
        adt_wr32(mark, 0xdc51);
        return;
    }
    pool = adt_child(chosen, "manifest-properties");
    pmgr = adt_child(arm_io, "pmgr");
    if (pool == 0 || pmgr == 0) {
        adt_wr32(mark, 0xdc52);
        return;
    }
    states = adt_prop(pmgr, "voltage-states2");
    if (states == 0 || adt_prop_size(states) != DCS_ENTRY) {
        adt_wr32(mark, 0xdc53);     /* absent, or already more than one */
        return;
    }

    count = adt_rd32(pool);
    prop = pool + 8;
    for (i = 0; i < count; i++) {
        if (adt_name_starts(prop, "UnusedStringProperty") &&
            adt_prop_size(prop) >= 64u) {
            donor = prop;
            break;
        }
        prop = adt_prop_next(prop);
    }
    if (donor == 0) {
        adt_wr32(mark, 0xdc54);
        return;
    }

    from = adt_prop_next(donor);
    end = states + ADT_PROP_HEADER + DCS_ENTRY;
    if (from >= end) {
        adt_wr32(mark, 0xdc55);     /* the donor has to come first */
        return;
    }

    raw = adt_rd32(donor + ADT_NAME_LEN);
    adt_wr32(
        donor + ADT_NAME_LEN,
        (raw & 0x80000000u) | ((raw & 0x7fffffffu) - DCS_NEED)
    );
    to = adt_prop_next(donor);

    for (uint8_t *s = from, *d = to; s < end; s++, d++) {
        *d = *s;
    }
    states -= DCS_NEED;

    /*
     * The state iBoot did publish stays as it published it, and the rest
     * climb from it: what distinguishes these entries on the rails that have
     * several is the voltage alone, five millivolts apart.
     */
    freq = adt_rd32(states + ADT_PROP_HEADER);
    volt = adt_rd32(states + ADT_PROP_HEADER + 4u);
    raw = adt_rd32(states + ADT_NAME_LEN);
    adt_wr32(
        states + ADT_NAME_LEN,
        (raw & 0x80000000u) | (DCS_STATES * DCS_ENTRY)
    );
    for (i = 1; i < DCS_STATES; i++) {
        adt_wr32(states + ADT_PROP_HEADER + i * DCS_ENTRY, freq);
        adt_wr32(
            states + ADT_PROP_HEADER + i * DCS_ENTRY + 4u,
            volt + DCS_STEP * i
        );
    }

    adt_clean(donor, end);
    adt_wr32(mark, 0xdc5f);
    adt_clean(mark, mark + 4);
}

/* Where the host leaves extra boot-args: the scratch aperture that
 * sptm_handoff.S calls SCRATCH (0x00160000), one page past the mark. */
/*
 * How much room the kernel's own `boot_args.CommandLine[]` really has.
 *
 * This was 256 for a long time, which is the number an x86 habit supplies and
 * not one anything here measured.  The kernel does not bound the command line
 * at all -- `PE_parse_boot_argn_internal` scans a NUL-terminated string -- so
 * the only limit is where the array ends and `bootFlags` begins.
 *
 * `boot_args` in xnu-10002 (the build a real iPhone14,4 runs) is
 *
 *     Revision 0x00, virtBase 0x08, physBase 0x10, memSize 0x18,
 *     topOfKernelData 0x20, Boot_Video 0x28 (48 bytes), machineType 0x58,
 *     deviceTreeP 0x60, deviceTreeLength 0x68, CommandLine 0x6c,
 *     bootFlags, memSizeActual
 *
 * which puts CommandLine at 0x6c -- the offset this file already used and the
 * string really is there.  19A348's own copy of the struct is not in any
 * public source, so the kernel was asked instead:
 * `ml_get_booter_memory_size` reads both ends of the pair,
 *
 *     LDR X9, [X8,#0x2D8]     ; memSizeActual
 *     LDR X8, [X8,#0x18]      ; memSize
 *
 * so `bootFlags` sits at 0x2D0 and the command line runs 0x6c..0x2D0 -- about
 * 612 bytes.  512 is taken rather than all of it: it doubles the budget and
 * still stops well short of the two fields behind it, which are exactly the
 * things a run-on would corrupt silently (a wrong `memSizeActual` does not
 * say where it came from).
 */

void sptm_handoff_boot_args(uint64_t boot_args);
HANDOFF void sptm_handoff_boot_args(uint64_t boot_args)
{
    /*
     * io=0x36 is kIOLogProbe|kIOLogStart|kIOLogMatch|kIOLogConfig: each
     * matching thread narrates what it is on, and a start that returns says
     * how long it took -- so a start that never returns is the one with no
     * such line.  keepsyms=1 keeps the symbol table resident so a panic
     * backtrace names functions instead of addresses this end has to map by
     */
    /*
     * debug=0x2014e is the combination iOS itself uses, and the bit that
     * matters here is DB_KPRT: kprintf goes to the serial port.  IOLog is
     * only what drivers choose to say; the kernel's own account of loading a
     * kext, taking a lock, or giving up on one goes through kprintf, and
     * without this none of it is written down anywhere.  DB_NMI comes with
     * it, which is what makes a non-maskable interrupt enter the debugger
     * rather than be ignored -- the way to get a backtrace out of a boot that
     * has stopped without panicking.
     */
    /*
     * And what is left off.
     *
     * debug= is what PE_i_can_has_debugger answers on, and a development
     * kernel spends that answer on timing assertions this machine cannot
     * meet.  AppleT8110PMGR times its own APSC wait and panics past a budget
     * of a few milliseconds:
     *
     *     _waitAPSCPending takes too long exeution time 8176 us domain 2
     *     apscManSlpStartTsTmp 0, CurrentTime 3280323526 counter 0
     *
     * with counter zero -- the register it waits on was ready at once, and
     * only the wall clock ran over, which on a machine where every access is
     * an exit it always will.  Measured earlier, debug=0x2014e buys nothing
     * on this kernelcache: kprintf reaches the port and a release kernel has
     * almost nothing to say through it at this stage.
     */
    /*
     * And every machine timeout stretched, because none of them were written
     * for a machine where an access is an exit.
     *
     * ml-timeout-global-scale multiplies every MACHINE_TIMEOUT in the kernel;
     * it is a knob XNU provides for exactly this, so nothing has to be
     * patched.  The one that matters most is debug-ack, 240000 by default:
     * the panic path gives the other cores that long to answer its cross-call
     * and, when they do not,
     *
     *     Debugger synchronization timed out; waited 240000 nanoseconds
     *     !! debugger synchronization failed, no stackshot !!
     *
     * -- and the stackshot is the only thing that says what every blocked
     * thread is waiting for.  Force-halting the stragglers does not undo it:
     * the gate is debug_ack_timeout_count, which the timeout has already
     * raised.  The cores do answer here, just not inside 240 microseconds.
     */
    /*
     * And the guest hypervisor turned off, because this machine cannot host
     * one.
     *
     * XNU brings its own hypervisor up at boot -- the switch is a boot-arg
     * called `hypervisor` and its default, written into the stack slot before
     * PE_parse_boot_argn is even called, is 1 -- and the first thing it does
     * per core is read ICH_VTR_EL2 and insist on eight list registers and five
     * priority bits.  Read at EL2 on this machine, a few instructions before
     * the kernel gets the core, that register is zero and so is ICC_SRE_EL2:
     * there is no GICv3 virtual CPU interface here at all.  The guest sees the
     * same zero and stops at
     *
     *     panic(cpu 0): hv_vcpu.c:1789 Assertion failed:
     *         ICH_VTR_LISTREGS(vtr) == (ARM_GUEST_ICH_LR_COUNT - 1)
     *
     * before it has a console to say so on.  Answering the register with a
     * number it likes would be a lie that holds exactly until something tries
     * to inject through a list register that is not there; the kernel already
     * has a switch for a machine that cannot run a guest, so use it.  Nothing
     * in a restore asks for one.
     */
    /*
     * And the sealed system volume's root hash allowed to differ.
     *
     * The restore that laid this drive out was driven with
     * `SealedSystemVolumeAuthenticate` false, so `apfs_sealvolume` sealed from
     * what was on the disk rather than from the signed `root_hash.img4`: the
     * seal is self-consistent and its hash is not the one Apple signed.  iBoot
     * compares the two at `apfs_fs.c:150` and XNU again at
     * `apfs_vfsops.c:539` -- "APFSX: could not validate root hash
     * Authentication error (80)".  This is the switch restored itself writes
     * to NVRAM when it skips sealing entirely; it turns off
     * `is_root_hash_authentication_required` and the check in
     * `handle_snapshot_mount` alike.
     */
    /*
     * And code signing out of the way.
     *
     * The installed system has no shell -- `/bin` holds `df` and `ps` and
     * nothing else -- so anything this project wants to run inside it is a
     * binary we build and ad-hoc sign, and AMFI refuses those.  All five
     * switches exist in this kernelcache, and they are honoured only when
     * `PE_i_can_has_debugger()` agrees; it does here, because
     * /chosen/debug-enabled reads one, which is the single property
     * `debug_enabled` is built from (kc:0xFFFFFFF00A9F2138).  The production
     * status is a separate field and a separate decision -- see
     * sptm_handoff_production_status, which deliberately sets it to one.
     *
     * This board is a development part by construction and there is nothing
     * here for a signature to protect.
     */
    static const char add[] =
        " serial=3 -v keepsyms=1 io=0xc00000036"
        " ml-timeout-global-scale=64 hypervisor=0"
        " allow-root-hash-mismatch=1"
        " amfi_get_out_of_my_way=1 cs_enforcement_disable=1"
        /*
         * And the ANE out of CLPC's arithmetic.
         *
         * ApplePMGR publishes DVFMStates -- one dictionary per fabric state,
         * each carrying StateID, Frequency, Voltage and fANE0/1/2 -- and
         * AppleCLPC builds the ANE's operating points out of the fANE0 column,
         * keeping the values that are not zero
         * (AppleCLPC/CLPC/source/fabric_dvfm_table.cpp).  Every one of them is
         * zero here, so the count is zero, and Apple's own zero-count path
         * zeroes its two stack arrays and then reads the *last* element of one:
         *
         *     subs w9, w21, #1             ; count == 0 -> 0xffffffff
         *     add  x13, sp, #0x360
         *     ldr  s1, [x13, w9, uxtw#2]   ; 0x3ffffffc past the array
         *
         * -- "Kernel data abort ... esr 0x96000006 far 0xffffffefd0e838ac",
         * four seconds into launchd on whichever core the IOConfigThread is on,
         * reached from IOService::startCandidate (24A437).
         * Patching the reads only walks the abort forward a site at a time:
         * three of them, in two functions, over two boots.
         *
         * CLPC already has the switch for a part with no ANE.  It reads
         * `ane-disabled` as a property and then lets a boot-arg of the same
         * name override it -- `PE_parse_boot_argn("ane-disabled", &v, 4)` at
         * 0xfffffff0093102d4 and 0x...0314 in the 24A437 kext -- and keeps the
         * result in the byte it tests where those tables are used.  Nothing in
         * a restore clocks an ANE.
         */
        " ane-disabled=1"
        /*
         * And a launchd job cache we are allowed to edit.
         *
         * iOS's launchd does not scan /System/Library/LaunchDaemons: the
         * `launchd_cache_loader` boot task hands it a prebuilt
         * /System/Library/xpc/launchd.plist, attaches the detached signature
         * beside it, and the kernel then page-validates the file -- so an
         * edited cache dies as
         *
         *     CODE SIGNING: rejecting invalid page ... tainted:1
         *     Boot task failed: launchd_cache_loader - OS_REASON_CODESIGNING
         *     panic: userspace panic: boot task failure
         *
         * even with cs_enforcement off, because the file is signed and
         * validated rather than merely allowed.  The loader has its own way
         * in: it reads `launchd_unsecure_cache=` out of `kern.bootargs` and,
         * when set, logs "Using unsecure cache: %s" and takes a branch that
         * loads the same default path **without attaching or checking the
         * signature**.
         *
         * It is a **number**, not a path.  The lookup is
         * `strstr(bootargs, "launchd_unsecure_cache=")` followed by
         * `strtoul(..., 10)`, returning the default of zero when nothing
         * parses -- so a path there reads as "off", which is what the first
         * attempt did.  The cache stays at /System/Library/xpc/launchd.plist
         * and is simply ours to edit.
         */
        " launchd_unsecure_cache=1"
        /*
         * And the userspace watchdog, which otherwise ends every boot after
         * about three minutes:
         *
         *     panic: userspace watchdog timeout: no successful checkins from
         *       SpringBoard since monitoring enabled by backboardd
         *       (190 seconds ago)
         *
         * backboardd arms it once it is up and SpringBoard is expected to
         * check in; with no display pipe SpringBoard never gets far enough to,
         * so the machine shoots itself while whatever is being debugged is
         * still running.  It reads as the guest hanging, or as a command over
         * usbmux producing no output, which is what it looked like for several
         * boots.  `wdt=-1` disables it.
         */
        " wdt=-1"
        /*
         * And the WKdm compressor in software, because the hardware one is not
         * modelled.
         *
         * XNU's compressor uses the WKDMC/WKDMD instructions when the part has
         * them, and this one answers neither: the accelerator's own guard
         * counted a hundred thousand consecutive traps on one page and stopped
         * the machine rather than spin forever --
         *
         *     HVF: vcpu 1 WKDMC at pc 0xfffffff04f812a3c traps every time
         *     (100000 in a row on one page); stopping the machine --
         *     boot the guest with -vm_compressor_wk_sw
         *
         * Measured 2026-09-28 on 24A437: the restore ran
         * to `ASR RESTORE PROGRESS: 31%` and froze, `VM status: paused
         * (internal-error)`, with three cores parked on a WFE lock loop behind
         * the one in the compressor.  Nothing before that boot had reached
         * enough memory pressure to enter it, which is why it appeared only
         * once update_aht and commit_sep_os were out of the way.
         *
         * `vm_compressor_wk_sw` is XNU's own switch for a part without the
         * instructions, so the compressor does the same work in C.
         */
        " -vm_compressor_wk_sw";
    char *line = (char *)(boot_args + BA_COMMAND_LINE);
    unsigned n = 0;
    unsigned i;

    while (n < BA_COMMAND_LEN && line[n] != '\0') {
        n++;
    }
    /*
     * And say so when it does not fit.
     *
     * `BA_COMMAND_LEN` is the budget and the loop below simply stops at it,
     * which is a silent truncation: a run once ended `... cs_enforcement_
     * disable=1 launchd`, the last argument cut in half, and the only symptom
     * was that the thing it switched on did not happen.  A breadcrumb costs
     * nothing and names it immediately.
     */
    for (i = 0; i + 1 < sizeof(add) && n + 1 < BA_COMMAND_LEN; i++) {
        line[n++] = add[i];
    }
    if (n < BA_COMMAND_LEN) {
        line[n] = '\0';
    }
    if (i + 1 < sizeof(add)) {
        say("OpenBootROM: boot-args TRUNCATED, ");
        say_hex(sizeof(add) - 1 - i);
        say(" bytes lost\n");
    }
    /*
     * And whatever the host wants to add, without rebuilding this.
     *
     * Boot-args are how nearly every kernel protection is switched off for a
     * test boot, and they were being changed one at a time by editing this
     * file and rebuilding the ROM.  That is the wrong shape: QEMU already has
     * a way to put bytes at an address (`-device loader`), the same one the
     * handoff mark uses, and 0x0016_0000..0x001c_0000 is the aperture nothing
     * else has a use for.  So the host writes eight magic bytes and a
     * NUL-terminated string at BOOT_ARGS_EXTRA and this appends it.
     *
     * The budget is real: `BA_COMMAND_LEN` counts what iBoot
     * already put there, and a silent truncation once cost a boot -- the
     * breadcrumb above says how many bytes were lost, and it counts these
     * too.
     */
    {
        static const char magic[] = "OMVBARG";
        const volatile char *extra = (const volatile char *)BOOT_ARGS_EXTRA;
        unsigned k;
        unsigned lost = 0;

        for (k = 0; k < sizeof(magic) - 1; k++) {
            if (extra[k] != magic[k]) {
                break;
            }
        }
        if (k == sizeof(magic) - 1) {
            /*
             * Append, or replace the whole line.
             *
             * Appending alone is not enough on this board.  Measured once:
             * boot: the line arrives from iBoot already carrying `serial=3
             * -v keepsyms=1 io=... ml-timeout-global-scale=64 hypervisor=0
             * allow-root-hash-mismatch=1 ...`, this adds its own, and the
             * result is **255 of the 256 bytes** -- so whatever the host
             * wants is simply lost, and the same argument can appear twice
             * because nothing looks.  The byte after the magic says which:
             * 'R' means the host is supplying the entire line and what was
             * there is dropped, which is the only way to spend the budget on
             * purpose.
             */
            if (extra[sizeof(magic) - 1] == 'R') {
                n = 0;
            }
            for (k = 0; extra[sizeof(magic) + k] != '\0'; k++) {
                if (n + 1 < BA_COMMAND_LEN) {
                    line[n++] = extra[sizeof(magic) + k];
                } else {
                    lost++;
                }
            }
            if (n < BA_COMMAND_LEN) {
                line[n] = '\0';
            }
            if (lost != 0) {
                say("OpenBootROM: boot-args EXTRA TRUNCATED, ");
                say_hex(lost);
                say(" bytes lost\n");
            }
        }
    }
    adt_clean((uint8_t *)line, (uint8_t *)line + BA_COMMAND_LEN);
    __asm__ volatile("dsb sy" ::: "memory");
}

void sptm_handoff_patch(uint64_t entry, uint64_t scratch);
/*
 * An uploaded offset table, applied to whatever image the caller names.
 *
 * The monitor and the kernel are patched by matching encodings rather than by
 * offset, which is what makes those tables survive a rebuild, so this does not
 * replace them: it runs after, for the sites that have no encoding to match on
 * and that only the host can locate.
 */
HANDOFF static void patch_store_apply(uint32_t stage, uint8_t *base)
{
    uint32_t count = 0;
    const AVPImagePatch *patches = patch_store_patches(stage, &count);
    uint32_t i;

    if (patches == 0 || base == 0 || count == 0) {
        return;
    }
    for (i = 0; i < count; i++) {
        uint8_t *at = base + patches[i].offset;

        /* bit 31 names TXM-rx instead (patch_store_apply_txm) */
        if (patches[i].offset & PATCH_TXM_OFFSET_FLAG) {
            continue;
        }

        adt_wr32(at, patches[i].instruction);
        adt_clean(at, at + 4);
    }
    __asm__ volatile("ic ialluis" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/*
 * The kernel stage's rows for TXM.
 *
 * TXM links its own copy of libimg4, and on 24A437 the encoding scan above
 * (kc_force_img4_firmware_ok) matches nothing in it (txmimg4 0xf0c4), so the
 * cryptex trust cache an unsigned build's forged ticket covers is refused:
 * "TXM [Error]: TrustCache: selector: 12 | 0x02 | 0xA3" and launchd dies on
 * "Library not loaded: /usr/lib/libSystem.B.dylib".  A row of the kernel
 * stage whose offset has bit 31 set is an offset into TXM-rx instead of the
 * kernel text.  No kernel range reaches 2 GiB, and no table before 24A437
 * sets the bit, so every earlier table applies exactly as it did.
 */
HANDOFF static void patch_store_apply_txm(uint32_t stage, uint8_t *txm)
{
    uint32_t count = 0;
    const AVPImagePatch *patches = patch_store_patches(stage, &count);
    uint32_t i;
    uint32_t applied = 0;

    if (patches == 0 || txm == 0 || count == 0) {
        return;
    }
    for (i = 0; i < count; i++) {
        uint8_t *at;

        if ((patches[i].offset & PATCH_TXM_OFFSET_FLAG) == 0) {
            continue;
        }
        at = txm + (patches[i].offset & ~PATCH_TXM_OFFSET_FLAG);
        adt_wr32(at, patches[i].instruction);
        adt_clean(at, at + 4);
        applied++;
    }
    if (applied != 0) {
        __asm__ volatile("ic ialluis" ::: "memory");
        __asm__ volatile("dsb sy" ::: "memory");
        __asm__ volatile("isb" ::: "memory");
    }
}

HANDOFF void sptm_handoff_patch(uint64_t entry, uint64_t scratch)
{
    sptm_apply_patches((uint8_t *)entry, (uint8_t *)(scratch + 24));
    patch_store_apply(PATCH_STAGE_SPTM, (uint8_t *)entry);
}

/*
 * Take a page off gfx-data, so it no longer touches managed DRAM.
 *
 * The monitor walks /defaults/pmap-io-ranges and rejects an entry whose end
 * touches managed memory -- its test is `end >= managed_start`, not `>`, so a
 * region that merely abuts counts as overlapping -- and refuses to boot.  On
 * real hardware the carveouts below physBase leave a gap here; on this board
 * they do not, and gfx-data ends on exactly the byte managed memory starts at.
 *
 * Two places in the tree have to agree about that, because the monitor checks
 * them against each other: the io-range the table is built from, and
 * /arm-io/sgx/gfx-data-size, which uat_bootstrap turns into a page count and
 * registers.  Fixing one alone trades the overlap panic for "IO frame number
 * of pages does not match IO range number of pages".
 *
 * Both are reached by walking to the node that owns them.  The version of this
 * that scanned the whole tree for the property's name had to be careful that
 * the same base and size appear elsewhere as a plain region description; a
 * walk that names /defaults and /arm-io/sgx cannot find the wrong one.
 *
 * Breadcrumb at scratch + 92: 0xa5f0, plus 1 for the io-range and 2 for the
 * size once each is rewritten.  Neither is an error when it is absent -- only
 * a tree with a monitor behind it has either.
 */
#define GFX_DATA_BASE_HI    0x00000008u     /* the region is in DRAM     */
#define GFX_DATA_OLD        0x00098000u     /* as iBoot sizes gfx-data   */
#define GFX_DATA_NEW        0x00094000u     /* one page shorter          */
#define GFX_DATA_SIG        0x47554154u     /* 'GUAT', the entry's owner */

/*
 * The base is not the thing to match on.
 *
 * An entry is `{ uint64 addr; uint64 len; uint32 wimg; uint32 signature; }`,
 * 24 bytes, and the four words this scan reads are addr-lo, addr-hi, len-lo,
 * len-hi.  The first version keyed on addr-lo == 0x000b0000, which is where a
 * restore chain puts gfx-data -- and an installed boot puts it at
 * 0x8_018B_0000 instead, so the scan matched nothing, only /arm-io/sgx was
 * trimmed, and the monitor died in `iommu_bootstrap_register_io_range`:
 * "IO frame number of pages 37 does not match".  Measured on a frozen guest:
 * entry 61 of 62, `addr 0x8018b0000 len 0x98000 sig 'GUAT'`.
 *
 * The length and the owner are what identify it, and both are stable; the
 * address is not ours to predict.
 */

HANDOFF static void sptm_handoff_gfx_data(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 92);
    uint8_t *node;
    uint8_t *prop;
    uint32_t size;
    uint32_t i;
    unsigned done = 0;

    adt_wr32(mark, 0xa5f0);

    node = adt_child(root, "defaults");
    prop = node != 0 ? adt_prop(node, "pmap-io-ranges") : 0;
    if (prop != 0) {
        size = adt_prop_size(prop);
        for (i = 0; i + 24u <= size; i += 4u) {
            uint8_t *at = prop + ADT_PROP_HEADER + i;

            if (adt_rd32(at + 4) == GFX_DATA_BASE_HI &&
                adt_rd32(at + 8) == GFX_DATA_OLD &&
                adt_rd32(at + 12) == 0 &&
                adt_rd32(at + 20) == GFX_DATA_SIG) {
                adt_wr32(at + 8, GFX_DATA_NEW);
                adt_clean(at, at + 16);
                adt_wr32(mark + 4, (uint32_t)(uint64_t)at);
                done |= 1u;
                break;
            }
        }
    }

    node = adt_child(root, "arm-io");
    node = node != 0 ? adt_child(node, "sgx") : 0;
    prop = node != 0 ? adt_prop(node, "gfx-data-size") : 0;
    if (prop != 0 && adt_prop_size(prop) >= 8u &&
        adt_rd32(prop + ADT_PROP_HEADER) == GFX_DATA_OLD &&
        adt_rd32(prop + ADT_PROP_HEADER + 4) == 0) {
        adt_wr32(prop + ADT_PROP_HEADER, GFX_DATA_NEW);
        adt_clean(prop + ADT_PROP_HEADER, prop + ADT_PROP_HEADER + 8);
        done |= 2u;
    }
    adt_wr32(mark, 0xa5f0u | done);
}

/*
 * Do not ask for a GPU that powers itself off.
 *
 * `AGXArmFirmware::setupConfig` reads `gpu-idle-off-delay-ms` off
 * /arm-io/sgx.  This board's tree does not carry it, so the delay is zero,
 * which the driver says out loud -- "GPU Idle off delay (0)" -- and then acts
 * on: from the moment AGX starts, the GPU is to be idled off immediately and
 * continuously.
 *
 * A real GFX firmware powers the block down and brings it back.  The one this
 * machine provides plays the mailbox protocol and owns no hardware, so it can
 * do neither, and the driver is left waiting on a sleep that never arrives.
 * What that costs is not a stall: its progress timer eventually tears down
 * structures that are still in use, three or four pages at a time disappear
 * out of live kernel allocations, and the machine dies somewhere else
 * entirely -- in decmpfs, in a code-signing blob, in whichever innocent thing
 * touches the hole first, between twenty seconds and six minutes later.
 * Measured: with this property in place a boot ran nine minutes and counting
 * with AGX up, against every boot before it dying.
 *
 * So the delay is made long enough that the question never comes up.  This
 * comes out when the firmware side can service a power transition, and not
 * before; it is a statement about what this machine's GPU can do, which is
 * exactly what a device tree is for.
 *
 * Breadcrumb at scratch + 124: 0xa5e0, 0xa5e1 once the property is in, and
 * 0xa5e2 if the tree already had one and it was left alone.
 */
#define GPU_IDLE_OFF_DELAY_MS   3600000u

HANDOFF static void sptm_handoff_gpu_idle(uint64_t tree, uint64_t scratch)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 124);
    uint8_t *node;

    adt_wr32(mark, 0xa5e0);
    node = adt_child(root, "arm-io");
    node = node != 0 ? adt_child(node, "sgx") : 0;
    if (node == 0) {
        return;
    }
    if (adt_prop(node, "gpu-idle-off-delay-ms") != 0) {
        adt_wr32(mark, 0xa5e2);
        return;
    }
    if (adt_add_prop(root, node, "gpu-idle-off-delay-ms",
                     sizeof("gpu-idle-off-delay-ms") - 1u,
                     GPU_IDLE_OFF_DELAY_MS) != 0) {
        adt_wr32(mark, 0xa5e1);
        say("OpenBootROM: sgx gpu-idle-off-delay-ms <- 3600000\n");
    }
}

/*
 * voltage-states8, which this chain's iBoot never wrote.
 *
 * `ApplePMGR::initDriver` looks its own domain's table up by name and panics
 * when it is not there: "ApplePMGR: virtual void ApplePMGR::initDriver(
 * IOService *):1450 voltage-states8 not found" @ApplePMGR.cpp:1450, measured
 * 2026-09-26 on the second boot of the restore chain, right after the kernel
 * reaches ApplePMGR.  iBoot is what writes these tables -- see the note on
 * voltage-statesN above -- and the tree as shipped carries voltage-states0, 2
 * and 9 but not 8, so the one domain the kernel asks for is the one missing.
 *
 * The property is an array of states and nothing on this machine regulates a
 * rail, so one zeroed state is what voltage-states9 already carries; the driver
 * only has to find a table it can walk.  If the day comes that a domain's
 * voltages matter, this is the place that invented them.
 *
 * Breadcrumb at scratch + 204: 0xa5f0 added, 0xa5f1 written, 0xa5f2 already
 * present.  The scratch page is zeroed up to +120 only, so this slot is left
 * alone by the reset at the top of the run.
 */
/*
 * The node that carries a property, wherever it is in the tree.
 *
 * Depth is what an Apple tree has plenty of and a path walk has to know; the
 * property is what everything else here already searches by, so this walks the
 * children and asks each subtree the same question.
 */
HANDOFF static uint8_t *adt_find_prop_owner(uint8_t *node, const char *name)
{
    uint8_t *p = adt_after_props(node);
    uint32_t children = adt_rd32(node + 4);
    uint32_t i;

    if (adt_prop(node, name) != 0) {
        return node;
    }
    for (i = 0; i < children; i++) {
        uint8_t *hit = adt_find_prop_owner(p, name);

        if (hit != 0) {
            return hit;
        }
        p = adt_node_end(p);
    }
    return 0;
}

/*
 * Append a property, told how big the tree is allowed to get.
 *
 * `adt_add_prop_bytes` refuses to grow past ADT_TREE_CAPACITY, which is the
 * size the older builds' trees are given.  27.0's is larger -- the boot
 * arguments carry 0x44000 where the constant says 0x40000 -- so a step that
 * has to add anything here is refused for room that does not exist and looks
 * exactly like a step that found nothing to do.  The length is the truth and
 * the argument is where it comes from.
 */
HANDOFF static int adt_add_prop_capped(
    uint8_t *root,
    uint8_t *node,
    const char *name,
    unsigned length,
    const uint8_t *data,
    uint32_t size,
    uint32_t cap
)
{
    const uint32_t padded = (size + 3u) & ~3u;
    const uint32_t span = ADT_PROP_HEADER + padded;
    uint8_t *at = adt_after_props(node);
    uint8_t *end = adt_node_end(root);
    uint8_t *p;
    uint32_t c;

    if (cap == 0) {
        cap = ADT_TREE_CAPACITY;
    }
    if ((uint32_t)((end - root) + span) > cap) {
        return 0;
    }
    p = end;
    while (p > at) {
        p--;
        p[span] = *p;
    }
    adt_rename_prop(at, name, length);
    adt_wr32(at + 32, size);
    for (c = 0; c < padded; c++) {
        at[ADT_PROP_HEADER + c] = c < size ? data[c] : 0;
    }
    adt_wr32(node, adt_rd32(node) + 1u);
    return 1;
}

/*
 * `adt_resize_prop` against the tree's own length rather than the constant.
 *
 * Same body, one difference: the capacity is the caller's, because 27.0's tree
 * is 0x413d8 bytes against a constant of 0x40000 and every grow is refused
 * before it starts.
 */
HANDOFF static int adt_resize_prop_capped(
    uint8_t *root,
    uint8_t *prop,
    uint32_t want,
    uint32_t cap
)
{
    uint32_t have = adt_prop_size(prop);
    uint32_t old_span = (have + 3u) & ~3u;
    uint32_t new_span = (want + 3u) & ~3u;
    uint8_t *end = adt_node_end(root);
    uint8_t *from = prop + ADT_PROP_HEADER + old_span;
    uint8_t *p;
    uint32_t delta;
    uint32_t c;

    if (cap == 0) {
        cap = ADT_TREE_CAPACITY;
    }
    if (new_span == old_span) {
        adt_wr32(prop + 32, want);
        return 1;
    }
    if (new_span < old_span) {
        delta = old_span - new_span;
        for (p = from; p < end; p++) {
            *(p - delta) = *p;
        }
        adt_wr32(prop + 32, want);
        return 1;
    }
    delta = new_span - old_span;
    if ((uint32_t)((end - root) + delta) > cap) {
        return 0;
    }
    p = end;
    while (p > from) {
        p--;
        p[delta] = *p;
    }
    for (c = old_span; c < new_span; c++) {
        prop[ADT_PROP_HEADER + c] = 0;
    }
    adt_wr32(prop + 32, want);
    return 1;
}

/*
 * Write a property, whether the node already carries it or not.
 *
 * iBoot writes these tables itself when it can work them out, and what it
 * writes here is a stub: eight bytes where the part's own table is eighty.
 * The driver reads the count and the values out of whatever is there, so a
 * stub is not a smaller answer, it is a wrong one.
 */
HANDOFF static int adt_set_prop_words(
    uint8_t *root,
    uint8_t *node,
    const char *name,
    const uint32_t *words,
    unsigned count,
    uint32_t cap
)
{
    uint32_t size = count * 4u;
    unsigned length = 0;
    uint8_t *prop;
    unsigned i;

    while (name[length] != '\0') {
        length++;
    }
    prop = adt_prop(node, name);
    if (prop == 0) {
        return adt_add_prop_capped(
            root,
            node,
            name,
            length,
            (const uint8_t *)words,
            size,
            cap
        );
    }
    /* grown in place: the resize carries the move and the count */
    if (!adt_resize_prop_capped(root, prop, size, cap)) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        adt_wr32(prop + ADT_PROP_HEADER + i * 4u, words[i]);
    }
    return 1;
}

/*
 * voltage-states8, which this chain's iBoot never wrote.
 *
 * `ApplePMGR::initDriver` looks its own domain's table up by name and panics
 * when it is not there: "ApplePMGR: virtual void ApplePMGR::initDriver(
 * IOService *):1450 voltage-states8 not found" @ApplePMGR.cpp:1450, measured
 * 2026-09-26 on the second boot of the restore chain, right after the kernel
 * reaches ApplePMGR.  The same wake-up had already been had on the installed
 * chain, where the fix was the part's own table, ten
 * (frequency, 0xffffffff) pairs, written by the ROM at the handoff.
 *
 * A zeroed or stubbed table is not accepted either -- with eight bytes where
 * the driver expects eighty it read a request of its own making and asserted
 * "VENC-AVE-VNOM 110 request AVE.1 > 1" @ApplePMGR.cpp:2002.
 *
 * Breadcrumb at scratch + 204: 0xa5f0 entered, 0xa5f1 written, 0xa5f2 already
 * right, 0xa5f3 no node.  The scratch page is zeroed up to +120 only, so this
 * slot survives the reset at the top of the run.
 */
HANDOFF static void sptm_handoff_pmgr_states(uint64_t tree, uint64_t scratch)
{
    /*
     * The PMGR's own tables, as the part publishes them.
     *
     * iBoot builds these from binning data it reads off the chip -- sram
     * revision, per-cluster binning -- and what it can build on this machine
     * is a stub: eight bytes where the table is eighty or a hundred and
     * twenty.  ApplePMGR::initDriver walks them and asserts on every one it
     * cannot reconcile, one boot at a time:
     *
     *   voltage-states8 not found                              @ApplePMGR.cpp:1450
     *   VENC-AVE-VNOM 110 request AVE.1 > 1                    @ApplePMGR.cpp:2002
     *   device perf counter flag is set with perf counter V2   @ApplePMGR.cpp:2046
     *   die > 0 || cpuComplex->deviceID == kIDReserved         @ApplePMGR.cpp:2229
     *
     * The values below are the part's, read off a booted device and kept from
     * the earlier chain's notes (2026-08): (frequency, voltage) pairs,
     * with the voltage word naming the rail's class rather than a number --
     * 0x190/0x195/0x19a/0x19f for the clocked domains, 0x7d for sram, 0x2fd
     * for sram9, and 0xffffffff for the encoder rail that has no regulator.
     */
    static const uint32_t vs0[] = { 0x00000001, 0x0000019a, 0x00000001, 0x00000195,
                                    0x00000001, 0x0000019f, 0x00000001, 0x0000019f };
    static const uint32_t vs1[] = { 0x0001aaaa, 0x00000195, 0x000139b9, 0x00000195,
                                    0x0000d555, 0x00000195, 0x0000a19d, 0x00000195,
                                    0x00007efb, 0x0000019a };
    static const uint32_t vs1s[] = { 0x23c34600, 0x0000007d, 0x30a32c00, 0x0000007d,
                                     0x47868c00, 0x0000007d, 0x5e69ec00, 0x0000007d,
                                     0x7829b800, 0x0000007d };
    static const uint32_t vs5[] = { 0x000183e0, 0x00000190, 0x000118b3, 0x00000190,
                                    0x0000de38, 0x00000190, 0x0000b981, 0x00000190,
                                    0x0000a066, 0x00000190, 0x00008d47, 0x00000190,
                                    0x00007efb, 0x00000190, 0x000073f1, 0x00000190,
                                    0x00006aaa, 0x00000190, 0x00006339, 0x00000190,
                                    0x00005cc0, 0x00000190, 0x00005713, 0x00000190,
                                    0x0000520d, 0x00000190, 0x00005080, 0x00000190,
                                    0x00004f03, 0x00000190 };
    static const uint32_t vs5s[] = { 0x2756cd00, 0x0000007d, 0x365c0400, 0x0000007d,
                                     0x44aa2000, 0x0000007d, 0x52412100, 0x0000007d,
                                     0x5f210700, 0x0000007d, 0x6c00ed00, 0x0000007d,
                                     0x7829b800, 0x0000007d, 0x839b6800, 0x0000007d,
                                     0x8f0d1800, 0x0000007d, 0x99c7ad00, 0x0000007d,
                                     0xa4824200, 0x0000007d, 0xaf3cd700, 0x0000007d,
                                     0xb9f76c00, 0x0000007d, 0xbd8af300, 0x0000007d,
                                     0xc11e7a00, 0x0000007d };
    static const uint32_t vs8[] = { 0x1f78a400, 0xffffffff, 0x20e6da00, 0xffffffff,
                                    0x2c588a00, 0xffffffff, 0x39387000, 0xffffffff,
                                    0x433bea00, 0xffffffff, 0x47868c00, 0xffffffff,
                                    0x53af5700, 0xffffffff, 0x579e6b80, 0xffffffff,
                                    0x61a1e580, 0xffffffff, 0x73838880, 0xffffffff };
    static const uint32_t vs9[] = { 0x00000000, 0x0000007d, 0x14257880, 0x00000195,
                                    0x255f42c0, 0x00000195, 0x30291a00, 0x00000195,
                                    0x3aa6a600, 0x00000195, 0x43f30500, 0x00000195,
                                    0x4bd12e00, 0x0000019a, 0x4fc04280, 0x0000019a };
    static const uint32_t vs9s[] = { 0x00000000, 0x000002fd, 0x14257880, 0x000002fd,
                                     0x255f42c0, 0x000002fd, 0x30291a00, 0x000002fd,
                                     0x3aa6a600, 0x000002fd, 0x43f30500, 0x000002fd,
                                     0x4bd12e00, 0x000002fd, 0x4fc04280, 0x000002fd };
    static const uint32_t vs10[] = { 0x00000001, 0x00000195, 0x00000001, 0x0000019a,
                                     0x00000001, 0x0000019f };
    static const uint32_t vs11[] = { 0x00000001, 0x00000190 };
    static const struct {
        const char *name;
        const uint32_t *words;
        unsigned count;
    } tables[] = {
        { "voltage-states0",      vs0,  sizeof(vs0)  / 4u },
        { "voltage-states1",      vs1,  sizeof(vs1)  / 4u },
        { "voltage-states1-sram", vs1s, sizeof(vs1s) / 4u },
        /*
         * voltage-states2 is absent on purpose: sptm_handoff_dcs_states owns
         * it, and it runs earlier in this handoff.  It expands the single
         * entry iBoot publishes into the four the DCS rail carries; a
         * two-word row here fails the size check above (32 != 8), puts the
         * stub back, and the kernel asserts again with
         *
         *     "psRegIndex < _getMaxDevicePsRegSize()" @ApplePMGR.cpp:2090
         *
         * because the DCS device (CLPC-DCS-VNOM, id 0x11B) carries
         * psRegIndex 1, and one entry is not more than one.  Measured
         * 2026-09-26 on 24A437, frozen inside that assertion ladder with
         * voltage-states2 read back at eight bytes, { 1, 0x190 }.
         */
        { "voltage-states5",      vs5,  sizeof(vs5)  / 4u },
        { "voltage-states5-sram", vs5s, sizeof(vs5s) / 4u },
        { "voltage-states8",      vs8,  sizeof(vs8)  / 4u },
        { "voltage-states9",      vs9,  sizeof(vs9)  / 4u },
        { "voltage-states9-sram", vs9s, sizeof(vs9s) / 4u },
        { "voltage-states10",     vs10, sizeof(vs10) / 4u },
        { "voltage-states11",     vs11, sizeof(vs11) / 4u },
    };
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 204);
    uint32_t cap = adt_rd32((uint8_t *)(scratch + 208));
    uint8_t *node;
    unsigned i;
    unsigned done = 0;

    adt_wr32(mark, 0xa5f0);
    node = adt_find_prop_owner(root, "voltage-states9");
    if (node == 0) {
        adt_wr32(mark, 0xa5f3);
        return;
    }
    for (i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        uint8_t *have = adt_prop(node, tables[i].name);

        if (have != 0 && adt_prop_size(have) == tables[i].count * 4u) {
            continue;               /* iBoot built this one the same way */
        }
        if (adt_set_prop_words(root, node, tables[i].name, tables[i].words,
                               tables[i].count, cap)) {
            done++;
        }
    }
    /*
     * And the scalars beside them, which iBoot derives from the same binning
     * data it cannot read here.  Every one is a word, every value is the
     * part's, and the four a boot needs are the two performance states, the
     * board id the tunable header wants and the revision pair that goes with
     * it.  ApplePMGR::initDriver asserts on each of them in turn when they are
     * still zero -- "psRegGroup < _psRegGroupCount" @ApplePMGR.cpp:2089 is the
     * one that got this far with the tables filled in but the scalars not.
     */
    {
        static const struct {
            const char *name;
            uint32_t value;
        } scalars[] = {
            { "nominal-performance1",  0x0000d555u },
            { "boost-performance1",    0x000139b9u },
            { "tunableh-board-id",     0x00000008u },
            { "tunableh-major-rev",    0x00000001u },
            { "tunableh-minor-rev",    0x00000001u },
        };
        unsigned s;

        for (s = 0; s < sizeof(scalars) / sizeof(scalars[0]); s++) {
            uint8_t *have = adt_prop(node, scalars[s].name);

            if (have != 0 && adt_rd32(have + ADT_PROP_HEADER) ==
                             scalars[s].value) {
                continue;
            }
            if (adt_set_prop_words(root, node, scalars[s].name,
                                   &scalars[s].value, 1u, cap)) {
                done++;
            }
        }
    }
    adt_wr32(mark, 0xa5f1 | (done << 16));
    say("OpenBootROM: pmgr voltage/perf tables <- the part's own\n");
}

/*
 * Everything the handoff does, in order.
 *
 * The boot arguments are the only thing this is handed, and the tree hangs off
 * them: a kernel virtual address at +0x60 and a length at +0x68, turned into a
 * physical one with the virtual and physical bases at +0x08 and +0x10.  Every
 * step after that is independent and none of them is fatal -- a tree without
 * the node a step wants leaves its breadcrumb and the next step still runs.
 *
 * Returns nonzero when a monitor is next, which is what decides whether to arm
 * the trap for its guarded ERET.  The tree says which: /chosen/memory-map's
 * BootKC-rx is where iBoot put the kernelcache's executable text, and if the
 * address iBoot is about to jump to is inside it then the kernel is next and
 * there is no monitor at all.  That is 15.0.1, whose bootx enters the
 * kernelcache directly; 17.0 enters SPTM, which is nowhere near that range.
 *
 * Breadcrumbs, all in the scratch page: +0 the mark that says this is the
 * second pass, +8 the boot arguments, +16 the tree, +24 and +32 the monitor's
 * own patch state, +56..+80 one per tree edit, +84 and +88 the kernel, +92 the
 * gfx-data pair, +96 this.
 */
uint32_t sptm_handoff_run(uint64_t boot_args, uint64_t scratch, uint64_t entry);

HANDOFF static uint64_t adt_rd64(const uint8_t *p)
{
    return (uint64_t)adt_rd32(p) | ((uint64_t)adt_rd32(p + 4) << 32);
}

#define BA_VIRT_BASE    0x08u
#define BA_PHYS_BASE    0x10u
#define BA_MEM_SIZE     0x18u
/*
 * Boot_Video, boot_args+0x28, 48 bytes: v_baseAddr, v_display, v_rowBytes,
 * v_width, v_height, v_depth.  This is what `initialize_screen` reads, and
 * what the kernel's console and the boot progress draw into.
 */
#define BA_VIDEO_BASE   0x28u
#define BA_VIDEO_DISPLAY 0x30u
#define BA_VIDEO_ROWBYTES 0x38u
#define BA_VIDEO_WIDTH  0x40u
#define BA_VIDEO_HEIGHT 0x48u
#define BA_VIDEO_DEPTH  0x50u
#define BA_DEVICE_TREE  0x60u
#define BA_DT_LENGTH    0x68u

/*
 * The size iBoot would have picked with no panel described to it, and the
 * size the emulator's DCP model scans out by default.  640 x 1136 x 4 is
 * 2.9 MB, well inside the 8.7 the old vram region holds.
 */
#define BOOT_FB_WIDTH   640u
#define BOOT_FB_HEIGHT  1136u

/*
 * A framebuffer carveout the default framebuffer fits in.
 *
 * iBoot sizes /vram for the display it believes in, and with no panel
 * described to it on this machine that is the 640 x 1136 fallback: three
 * such buffers, 0x854000 bytes, at the top of DRAM between region-id-4 and
 * region-id-80 with nothing to spare on either side.  The kernel's
 * IOSurfaceRoot turns that node into the "PurpleGfxMem" region, and
 * IOMobileFramebufferAP::allocate_carveout clamps the default framebuffer
 * to what the region holds; layout_buffers then cuts the surface's height to
 * fit.  The DFB the DCP model asks for is 1125 x 2436 'BGRA' -- 4608 bytes a
 * row, and allocate_carveout asks for 1.15 x 1.15 of that on a d16 -- which
 * is 14.7 MB, so the window server would be handed a framebuffer a third
 * short of the panel.
 *
 * DRAM is exactly the 4 GB the tree says, so the room comes out of the
 * kernel's own range: the top 16 MB of region-id-24 becomes /vram, and
 * region-id-24 and boot-args memSize both shrink by the same amount, so
 * the kernel never learns those pages existed.  That is the arrangement
 * iBoot itself makes -- vram is outside gPhysBase..gPhysSize today too, and
 * the kernel maps it as device memory through the node, not the physmap.
 * iBoot's own framebuffer, and boot-args' video base, stay where they were:
 * that memory belongs to no region now and nothing will allocate it.
 *
 * Three things must agree before anything is written: the node's reg, the
 * carveout map's region-id-14, and memSize against region-id-24.  If any of
 * them does not, the tree is left as iBoot made it and the mark says which.
 */
#define VRAM_WANT       0x1000000ull

HANDOFF static void sptm_handoff_vram(
    uint64_t tree,
    uint64_t scratch,
    uint64_t boot_args
)
{
    uint8_t *root = (uint8_t *)tree;
    uint8_t *mark = (uint8_t *)(scratch + 116);
    uint8_t *ba = (uint8_t *)boot_args;
    uint8_t *vram = adt_child(root, "vram");
    uint8_t *map = adt_child(adt_child(root, "chosen"), "carveout-memory-map");
    uint8_t *reg;
    uint8_t *r14;
    uint8_t *r24;
    uint64_t old_base, old_size, k_base, k_size, new_base;
    uint64_t taken = VRAM_WANT;

    adt_wr32(mark, 0xf0);
    if (vram == 0 || map == 0) {
        adt_wr32(mark, 0xf1);
        return;
    }
    reg = adt_prop(vram, "reg");
    r14 = adt_prop(map, "region-id-14");
    r24 = adt_prop(map, "region-id-24");
    if (reg == 0 || r14 == 0 || r24 == 0 || adt_prop_size(reg) != 16u ||
        adt_prop_size(r14) != 16u || adt_prop_size(r24) != 16u) {
        adt_wr32(mark, 0xf2);
        return;
    }
    old_base = adt_rd64(reg + ADT_PROP_HEADER);
    old_size = adt_rd64(reg + ADT_PROP_HEADER + 8);
    if (old_size >= VRAM_WANT) {
        adt_wr32(mark, 0xf3);
        return;
    }
    if (adt_rd64(r14 + ADT_PROP_HEADER) != old_base ||
        adt_rd64(r14 + ADT_PROP_HEADER + 8) != old_size) {
        adt_wr32(mark, 0xf4);
        return;
    }
    k_base = adt_rd64(r24 + ADT_PROP_HEADER);
    k_size = adt_rd64(r24 + ADT_PROP_HEADER + 8);
    if (adt_rd64(ba + BA_MEM_SIZE) != k_size || k_size <= 4u * VRAM_WANT) {
        adt_wr32(mark, 0xf5);
        return;
    }
    /*
     * Apple's dcpf gets its sixteen megabytes from here too, below vram's;
     * see dcp_carveout.  The constant the rest of the tree is built on is
     * checked against the arithmetic rather than trusted, because a base the
     * kernel still owns is memory two things write.
     */
    if (rom_real_dcp_firmware()) {
        if (k_base + k_size - VRAM_WANT - DCP_FW_REAL_SIZE
                != DCP_FW_REAL_BASE) {
            adt_wr32(mark, 0xf6);
            say("OpenBootROM: DCP_FW_REAL_BASE is not ");
            say_hex(k_base + k_size - VRAM_WANT - DCP_FW_REAL_SIZE);
            say(", the firmware is on memory the kernel owns\n");
            return;
        }
        taken += DCP_FW_REAL_SIZE;
    }
    new_base = k_base + k_size - VRAM_WANT;

    adt_wr64(r24 + ADT_PROP_HEADER + 8, k_size - taken);
    adt_wr64(ba + BA_MEM_SIZE, k_size - taken);
    adt_wr64(reg + ADT_PROP_HEADER, new_base);
    adt_wr64(reg + ADT_PROP_HEADER + 8, VRAM_WANT);
    adt_wr64(r14 + ADT_PROP_HEADER, new_base);
    adt_wr64(r14 + ADT_PROP_HEADER + 8, VRAM_WANT);

    /*
     * And now tell the kernel where it may draw.
     *
     * `Boot_Video` is all zeros here: iBoot on this machine has no display
     * driver, so it never allocated a framebuffer and never filled the
     * struct -- measured, the display model reads the base iBoot's /vram
     * named and finds nothing but zeros for a whole boot.  With it empty
     * `initialize_screen` has no framebuffer, the kernel's console goes
     * nowhere, and `-v` puts nothing on the panel however far the boot gets.
     *
     * The region iBoot's /vram used to name is the right place for it: the
     * lines above moved /vram off it and shrank the kernel's own range, so
     * those pages now belong to no region and nothing will allocate them.
     * `old_size` is 8.7 MB and 640 x 1136 x 4 is 2.9, so it fits with room
     * over.
     *
     * The emulator's DCP model scans the same base out (`boot-fb`), so what
     * the kernel draws here is what reaches the screen.  This is the one path
     * to the panel that does not go through CoreAnimation, which cannot
     * composite at all on a machine with no GPU and no completed scaler
     * transform.
     */
    if (old_size >= (uint64_t)BOOT_FB_WIDTH * BOOT_FB_HEIGHT * 4u) {
        adt_wr64(ba + BA_VIDEO_BASE, old_base);
        adt_wr64(ba + BA_VIDEO_DISPLAY, 1);
        adt_wr64(ba + BA_VIDEO_ROWBYTES, (uint64_t)BOOT_FB_WIDTH * 4u);
        adt_wr64(ba + BA_VIDEO_WIDTH, BOOT_FB_WIDTH);
        adt_wr64(ba + BA_VIDEO_HEIGHT, BOOT_FB_HEIGHT);
        adt_wr64(ba + BA_VIDEO_DEPTH, 32);
        say("OpenBootROM: Boot_Video ");
        say_hex(old_base);
        say(" 640x1136x32\n");
    }
    adt_wr32(mark, 0xff);

    say("OpenBootROM: vram ");
    say_hex(old_base);
    say(" + ");
    say_hex(old_size);
    say(" -> ");
    say_hex(new_base);
    say(" + ");
    say_hex(VRAM_WANT);
    say(", kernel RAM ");
    say_hex(k_size - taken);
    say("\n");
}

HANDOFF uint32_t sptm_handoff_run(
    uint64_t boot_args,
    uint64_t scratch,
    uint64_t entry
)
{
    uint8_t *pad = (uint8_t *)scratch;
    uint8_t *mark = pad + 96;
    const uint8_t *ba = (const uint8_t *)boot_args;
    uint64_t dt_va;
    uint64_t tree;
    uint32_t dt_len;
    uint32_t monitor = 1;
    uint8_t *root;
    uint32_t i;

    /* A boot's marks, so nothing read back here is left from the last one. */
    for (i = 16u; i < 120u; i += 4u) {
        adt_wr32(pad + i, 0);
    }
    adt_wr64(pad + 8, boot_args);
    adt_wr32(mark, 0xa5a0);

    say("OpenBootROM: handoff, boot-args ");
    say_hex(boot_args);
    say(" next ");
    say_hex(entry);
    say("\n");

    if (boot_args == 0) {
        adt_wr32(mark, 0xa5a1);
        return monitor;
    }
    dt_va = adt_rd64(ba + BA_DEVICE_TREE);
    dt_len = adt_rd32(ba + BA_DT_LENGTH);
    if (dt_va == 0 || dt_len == 0) {
        adt_wr32(mark, 0xa5a2);
        return monitor;
    }
    tree = dt_va - adt_rd64(ba + BA_VIRT_BASE) + adt_rd64(ba + BA_PHYS_BASE);
    adt_wr64(pad + 16, tree);
    /*
     * How big the tree is allowed to be, for the steps that have to grow it.
     * 27.0's is 0x44000 where the constant in the append helpers says 0x40000.
     */
    adt_wr32(pad + 208, dt_len);
    root = (uint8_t *)tree;

    {
        uint64_t at;
        uint64_t len;

        if (kc_text_range(root, &at, &len) != 0 &&
            entry >= at && entry < at + len) {
            monitor = 0;
        }
    }
    adt_wr32(mark, 0xa5a3 | (monitor << 8));

    say("OpenBootROM: handoff tree ");
    say_hex(tree);
    say(monitor != 0 ? ", monitor next\n" : ", kernel next\n");

    /*
     * What the virtual GIC reports about itself, read where the kernel will
     * read it: at EL2, on this machine, a few instructions before the kernel
     * gets the core.
     *
     * XNU asks the same question the moment its own hypervisor initialises --
     * hv_vcpu.c wants eight list registers and five priority bits and panics
     * on anything else -- and that read is served below the VMM, so nothing on
     * the host side can see the answer.  Here it can: this is the same
     * exception level and the same register.
     */
    {
        uint64_t vtr = 0;
        uint64_t sre = 0;

        __asm__ volatile("mrs %0, s3_4_c12_c11_1" : "=r"(vtr));
        __asm__ volatile("mrs %0, s3_4_c12_c9_5" : "=r"(sre));
        say("OpenBootROM: ICH_VTR_EL2 ");
        say_hex(vtr);
        say(" ICC_SRE_EL2 ");
        say_hex(sre);
        say("\n");
    }

    /* The tree, one edit at a time. */
    sptm_handoff_publish_lp_smb(tree, scratch);
    sptm_handoff_dcs_states(tree, scratch);
    sptm_handoff_smc_preloaded(tree, scratch);
    sptm_handoff_say_bus(tree);
    sptm_handoff_ignore_devices(tree, scratch);
    sptm_handoff_serial_dock_channel(tree, scratch);
    sptm_handoff_usb_device_enable(tree, scratch);
    sptm_handoff_dev_board(tree, scratch);
    sptm_handoff_supports_apt(tree, scratch);
    sptm_handoff_production_status(tree, scratch);
    sptm_handoff_ans_firmware(tree, scratch);
    sptm_handoff_dcp_firmware(tree, scratch);
    sptm_handoff_dcp_local_power(tree, scratch);
    sptm_handoff_display_pitch(tree, scratch);
    sptm_handoff_vram(tree, scratch, boot_args);
    sptm_handoff_watchdog_no_pmu(tree, scratch);
    sptm_handoff_sep_skip_lynx(tree, scratch);
    sptm_handoff_clpc_no_ane(tree, scratch);
    sptm_handoff_root_hash_mismatch(tree, scratch);
    sptm_handoff_no_eapfs(tree, scratch);
    /*
     * NOT called.  Measured 2026-09-04, both ways.
     *
     * With the property present the keystore uses the fake key
     * 0xffffffff and sks panics in its key check.  With it masked the
     * panic goes away -- but `AppleEffaceableStorage` never appears
     * (there is no such node in the tree at all), the wait is bounded
     * at 5 s + 55 s and then gives up, and the next thing that happens
     * is
     *
     *     aks_migrate_SEPUUID2b_to_classM_key:1963: fetch wrapped
     *         media key (uuid = 750FADB4-...), ret = 6
     *     migrate_media_keys_if_needed:1261: disk0s1 media key
     *         migration failed for AES, error = 6
     *     mount: failed to migrate Media Keys, error = c006
     *
     * so the data volume does not mount and the boot loops.  Both arms
     * fail on the same fact -- there is no effaceable storage here --
     * and the fake-key arm is the one Apple ships, so that is the one
     * to make work.  Kept, not called.
     */
    sptm_handoff_quiet_payload_usb(scratch);

    /* The GPU, which must not be told to power itself off. */
    sptm_handoff_gpu_idle(tree, scratch);

    /* And the PMGR tables iBoot writes and this chain's iBoot does not. */
    sptm_handoff_pmgr_states(tree, scratch);

    /* The kernel, wherever iBoot left it. */
    sptm_handoff_kernel_patch(tree, scratch);

    /* And the two the monitor alone cares about. */
    if (monitor != 0) {
        sptm_handoff_gfx_data(tree, scratch);
        sptm_handoff_patch(entry, scratch);
    }

    /*
     * Where the guarded code is, said rather than left to be guessed.
     *
     * The hypervisor holds an interrupt back while the guest is inside the
     * privileged layer, because this machine cannot deliver one there: an
     * exception taken in guarded state belongs in VBAR_GL with ELR_GL and
     * SPSR_GL, and the bank those live in is not reachable from outside, so
     * the level lands in the kernel's own vector instead and the layer's
     * nesting depth never recovers.  For that to work it has to know which
     * addresses are the layer's, and it has no way to find out for itself:
     * the guess it makes -- the lowest kernel band the guest has ever run in
     * -- is SPTM's on 17.0 by the accident of how three images are linked,
     * and XNU's own on a boot that has no separate monitor at all.
     *
     * Here both are known.  With a monitor it is where iBoot is about to
     * jump, taken to its band so the three images that share the slide are
     * covered.  Without one the layer is PPL, which is segments of the
     * kernelcache, and /chosen/memory-map names Kernel-__PPLTEXT exactly --
     * turned into a virtual address with the same pair that placed the tree.
     *
     * Two writes, base then length, and the length is what completes the
     * pair: a half-written range would read as a layer that starts somewhere
     * and never ends.
     */
    {
        uint64_t lo = 0;
        uint64_t size = 0;

        if (monitor != 0) {
            /*
             * As a virtual address, because that is what asks the question.
             *
             * The handoff runs with the MMU off, so `entry` is physical, and
             * the gate that consults this holds a kernel pc.  A physical band
             * matches no pc it will ever see: the gate answered "not in the
             * monitor" everywhere, every standing level was delivered inside
             * SPTM through the kernel's own vector table, and the monitor
             * spun for ever on its nesting depth.  The PPL arm below converts
             * for exactly this reason; this one did not.
             */
            lo = (entry - adt_rd64(ba + BA_PHYS_BASE)
                        + adt_rd64(ba + BA_VIRT_BASE)) & ~0xfffffffull;
            size = 0x20000000ull;
        } else {
            uint64_t at, len;

            if (map_range(root, "Kernel-__PPLTEXT", &at, &len) != 0) {
                lo = at - adt_rd64(ba + BA_PHYS_BASE)
                        + adt_rd64(ba + BA_VIRT_BASE);
                size = len;
            }
        }
        if (lo != 0 && size != 0) {
            __asm__ volatile("msr s3_6_c15_c15_6, %0" :: "r"(lo));
            __asm__ volatile("msr s3_6_c15_c15_5, %0" :: "r"(size));
            say("OpenBootROM: guarded range ");
            say_hex(lo);
            say(" + ");
            say_hex(size);
            say("\n");
        } else {
            say("OpenBootROM: guarded range unknown\n");
        }
    }

    /* Last, the command line, which iBoot filtered on the way through. */
    sptm_handoff_boot_args(boot_args);
    adt_wr32(mark, 0xa5af | (monitor << 8));

    say("OpenBootROM: handoff done, kernel ");
    say_hex((uint64_t)adt_rd32(pad + 84));
    say(" patched ");
    say_hex((uint64_t)adt_rd32(pad + 88));
    say(" text ");
    say_hex(((uint64_t)adt_rd32(pad + 140) << 32) | adt_rd32(pad + 144));
    /* How many sites the encoding scan rewrote in the monitor.  The two
     * chains do not run the same SPTM -- a restore boots
     * Ap_RestoreSPTM.dev, an installed system the release
     * Ap,SecurePageTableMonitor -- so a scan tuned on one can silently
     * match nothing in the other, and the monitor then halts in
     * _env_panic with no console to say why. */
    say(" sptm ");
    say_hex((uint64_t)adt_rd32(pad + 24));
    say(" rootvp ");
    say_hex((uint64_t)adt_rd32(pad + 148));
    /* the other three kernel rewrites, so a scan that matched nothing is
     * visible instead of silent */
    say(" rhash ");
    say_hex((uint64_t)adt_rd32(pad + 52));
    say(" livefs ");
    say_hex((uint64_t)adt_rd32(pad + 48));
    say(" img4 ");
    say_hex((uint64_t)adt_rd32(pad + 120));
    say(" txmimg4 ");
    say_hex((uint64_t)adt_rd32(pad + 168));
    say("\n  OpenBootROM: TrustCache region at ");
    say_hex(((uint64_t)adt_rd32(pad + 176) << 32) | adt_rd32(pad + 180));
    say(" len ");
    say_hex((uint64_t)adt_rd32(pad + 184));
    say(" caches ");
    say_hex((uint64_t)adt_rd32(pad + 188));
    say(" off0 ");
    say_hex((uint64_t)adt_rd32(pad + 192));
    say(" mod0 v");
    say_hex((uint64_t)adt_rd32(pad + 196));
    say(" entries ");
    say_hex((uint64_t)adt_rd32(pad + 200));
    say("\n");
    return monitor;
}


/*
 * The sealed volume's root hash, refused in the kernel rather than in the tree.
 *
 * APFS decides with `apfs_root_hash_validation_is_disabled`, and it is one
 * question:
 *
 *     DT_property("/options", "allow-root-hash-mismatch", &obj)
 *     -> OSData, and the byte is not '0'
 *
 * `/options` is where NVRAM variables surface, restored writes the variable
 * there itself when it skips sealing, and this chain has neither: iBoot reads
 * the NVRAM partition into a buffer without importing it, so the property is
 * absent, the function returns null, and the mount panics
 *
 *     is_root_hash_authentication_required:478: root hash authentication is required
 *     "APFSX: could not validate root hash Authentication error (80)" @apfs_vfsops.c:539
 *
 * The seal itself is sound -- `apfs_sealvolume` built it from the disk because
 * the restore ran with `SealedSystemVolumeAuthenticate` false -- so the hash is
 * simply not the one Apple signed, and this says so once, here, instead of
 * editing the kernelcache on the drive for every boot.
 *
 * Located by its own body, not by an offset.  The four words below are the
 * `add x29, sp, #0x20 / str xzr, [sp, #8] / adrp x0, "/options" / add x0` that
 * open it, and they occur exactly once in 19A348's development kernelcache;
 * the two before them are checked as well, so a build that moves the function
 * fails to match rather than patching the wrong place.
 */
/*
 * Do not panic when the root volume is not authenticated.
 *
 * `bsd_init` mounts the root and then asks whether the volume authenticated;
 * on an unsealed disk it did not, and the answer is
 *
 *     panic: rootvp not authenticated after mounting @bsd_init.c:969
 *
 * Our disk is deliberately unsealed -- the restore is run with
 * `SealSystemVolumeDuringRestore = false` so the System volume stays writable
 * for the dyld-shared-cache and CFW work, and `apfs_sealvolume` does not
 * complete in this environment anyway.  21A329 also has no way back: its
 * `is_root_hash_authentication_required` returns 0 only on a NeRD boot, and
 * the "development kext may skip" branch 19A348 had is gone, so swapping in a
 * development kernelcache would not help.
 *
 * vphone-cli's own survey of these patches lists the same three for an
 * unsealed volume -- `_apfs_vfsop_mount`'s snapshot check,
 * `_authapfs_seal_is_broken`, and this one -- and calls this "skip rootvp
 * not-authenticated panic".  This is that patch.
 *
 * Located by encoding, so no offset.  This shape is the authenticated
 * indirect-call form used by 21A329.  19A348's release kernel uses a direct
 * VNOP_IOCTL call instead; its explicit patch table uses the host's semantic
 * kernel locator rather than adding another build-shaped ROM scan.
 *
 *     d2860f51   mov   x17, #0x307a
 *     d73f0911   blraa x8, x17
 *     35001240   cbnz  w0, <the panic block>
 *
 * Breadcrumb at scratch + 148.
 */
HANDOFF static void kc_skip_rootvp_panic(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark
)
{
    static const uint32_t body[3] = {
        0xd2860f51u, 0xd73f0911u, 0x35001240u
    };
    uint64_t at;

    adt_wr32(mark, 0xf0b0);
    if (base == 0 || size < 16) {
        adt_wr32(mark, 0xf0b1);
        return;
    }
    for (at = 0; at + 12 <= size; at += 4) {
        if (adt_rd32(base + at) != body[0] ||
            adt_rd32(base + at + 4) != body[1] ||
            adt_rd32(base + at + 8) != body[2]) {
            continue;
        }
        adt_wr32(base + at + 8, KC_NOP);
        adt_clean(base + at + 8, base + at + 12);
        adt_wr32(mark, 0xf0bf);
        return;
    }
    adt_wr32(mark, 0xf0b3);                                 /* no match */
}




HANDOFF static void kc_force_root_hash_disabled(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark
)
{
    static const uint32_t body[4] = {
        0x910083fdu, 0xf90007ffu, 0xd0feb0e0u, 0x912d7000u
    };
    uint64_t at;

    adt_wr32(mark, 0xf0a0);
    if (base == 0 || size < 32) {
        adt_wr32(mark, 0xf0a1);
        return;
    }
    for (at = 16; at + 16 <= size; at += 4) {
        if (adt_rd32(base + at) != body[0] ||
            adt_rd32(base + at + 4) != body[1] ||
            adt_rd32(base + at + 8) != body[2] ||
            adt_rd32(base + at + 12) != body[3]) {
            continue;
        }
        if (adt_rd32(base + at - 16) != 0xd503237fu ||      /* pacibsp */
            adt_rd32(base + at - 12) != 0xd100c3ffu) {      /* sub sp, sp, #0x30 */
            adt_wr32(mark, 0xf0a2);
            return;
        }
        adt_wr32(base + at - 16, 0x52800020u);              /* mov w0, #1 */
        adt_wr32(base + at - 12, 0xd65f03c0u);              /* ret */
        adt_clean(base + at - 16, base + at - 8);
        adt_wr32(mark, 0xf0af);
        return;
    }
    adt_wr32(mark, 0xf0a3);                                 /* no match */
}

/*
 * Root the live System volume instead of its snapshot.
 *
 * `apfs_vfsop_mount` logs "Rooting from snapshot with xid 89" and mounts
 * `com.apple.os.update-<manifest hash>@/dev/disk0s1s1` read-only, which means
 * every edit made to the System volume from the host is invisible to the
 * running system -- the snapshot was taken when the restore sealed the volume
 * and nothing since has changed it.  That is the wall in front of installing
 * anything, patching the dyld shared cache included.
 *
 * APFS has a switch for exactly this.  `apfs_root_snapshot_select` asks
 * `apfs_root_to_snapshot_is_disabled()`, which is the same three lines as
 * `apfs_root_hash_validation_is_disabled` with a different name:
 *
 *     DT_property("/options", "root-live-fs", &obj)
 *     -> OSData, and the byte is not '0'
 *
 * and `/options` is empty here for the reason given above -- iBoot reads the
 * NVRAM partition without importing it -- so it always answers no.
 *
 * **Located by the property it reads, not by an immediate.**  Four functions
 * in this kernelcache open with the same six words, and the earlier patch
 * above told them apart by the `add x0, x0, #0xb5c` that completes the
 * "/options" pointer -- true today, and silently the wrong function the day a
 * build moves that string.  This one resolves the *second* adrp/add pair, the
 * one that loads the property name into x1, and compares the bytes.  The
 * arithmetic works on physical addresses because the image is loaded
 * page-aligned, so `pc & ~0xfff` names the same page either way.
 */
HANDOFF static uint64_t kc_adrp_add(uint8_t *at)
{
    uint32_t adrp = adt_rd32(at);
    uint32_t add = adt_rd32(at + 4);
    int64_t imm = (int64_t)(uint64_t)((((adrp >> 5) & 0x7ffffu) << 2) |
                                      ((adrp >> 29) & 3u));

    if (imm & ((int64_t)1 << 20)) {
        imm -= (int64_t)1 << 21;
    }
    return (((uint64_t)at & ~0xfffULL) + ((uint64_t)imm << 12)) +
           ((add >> 10) & 0xfffu);
}

HANDOFF static void kc_force_root_live_fs(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark
)
{
    static const char want[] = "root-live-fs";
    uint64_t at;

    adt_wr32(mark, 0xf0b0);
    if (base == 0 || size < 64) {
        adt_wr32(mark, 0xf0b1);
        return;
    }
    for (at = 16; at + 48 <= size; at += 4) {
        uint8_t *p = base + at;
        unsigned j;

        if (adt_rd32(p) != 0x910083fdu ||           /* add x29, sp, #0x20 */
            adt_rd32(p + 4) != 0xf90007ffu) {       /* str xzr, [sp, #8]  */
            continue;
        }
        if ((adt_rd32(p + 8) & 0x9f00001fu) != 0x90000000u ||   /* adrp x0 */
            (adt_rd32(p + 12) & 0xffc003ffu) != 0x91000000u) {  /* add  x0 */
            continue;
        }
        if (adt_rd32(p - 16) != 0xd503237fu ||      /* pacibsp            */
            adt_rd32(p - 12) != 0xd100c3ffu) {      /* sub sp, sp, #0x30  */
            continue;
        }
        for (j = 16; j <= 32; j += 4) {
            const char *name;
            unsigned k;

            if ((adt_rd32(p + j) & 0x9f00001fu) != 0x90000001u ||
                (adt_rd32(p + j + 4) & 0xffc003ffu) != 0x91000021u) {
                continue;                            /* adrp x1 / add x1 */
            }
            name = (const char *)kc_adrp_add(p + j);
            for (k = 0; k < sizeof(want); k++) {
                if (name[k] != want[k]) {
                    break;
                }
            }
            if (k != sizeof(want)) {
                break;                               /* a different one  */
            }
            adt_wr32(p - 16, 0x52800020u);           /* mov w0, #1       */
            adt_wr32(p - 12, 0xd65f03c0u);           /* ret              */
            adt_clean(p - 16, p - 8);
            adt_wr32(mark, 0xf0bf);
            return;
        }
    }
    adt_wr32(mark, 0xf0b3);                          /* no match         */
}

/*
 * Let the kernel's own image4 evaluation say yes.
 *
 * With /arm-io/dockchannel-mtp unmasked the AP tries to load the touch
 * processor's firmware and stops:
 *
 *     AppleImage4: authenticating firmware on chip: 4cc = mtpf, chip = sha2-384 arm ap
 *     AppleImage4: chip allows mix-n-match; ignoring previous stage manifest hash
 *     AppleImage4: trust evaluation failed: dr = -1, ct = 0xaaaaaaaa
 *     AIDImageDownloader::validationComplete() kFWValidationFailed
 *
 * `dr` and `ct` are two of the sentinels `img4_firmware_evaluate` writes over
 * its own locals on entry -- 0xffffffff and 0xaaaaaaaa -- so they are being
 * printed unset: the evaluation refused before it had decided anything.  It is
 * the same refusal `image4.property_match` answers on the other side of the
 * handoff, for the same reason: the ticket this machine hands on was issued
 * for another machine, so no property comparison it makes can ever hold.
 * hpas, lphp, mtfw and mtpf all fail this way, which is every runtime firmware
 * the machine downloads.
 *
 * Patched at the one place where the answer leaves the function:
 *
 *     mov x0, x21   ->   mov x0, #0
 *
 * The evaluation still runs in full.  That matters -- the caller goes on to
 * use the payload this function decoded, so returning early from the prologue
 * would hand it nothing.  x21 is where every path funnels its errno, and there
 * is exactly one epilogue.
 *
 * The three-word epilogue is shared with a second function, so the function is
 * found by its prologue -- those five words occur once in the whole image --
 * and the epilogue is then looked for inside it.
 */
HANDOFF static void kc_force_img4_firmware_ok(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark
)
{
    uint64_t at;

    adt_wr32(mark, 0xf0c0);
    if (base == 0 || size < 64) {
        adt_wr32(mark, 0xf0c1);
        return;
    }
    for (at = 28; at + 20 <= size; at += 4) {
        uint8_t *p = base + at;
        uint64_t j;
        uint32_t frame;

        /*
         * One function, built twice.
         *
         * 19A348 and 21A329 agree on the frame -- `add x29,sp,#0x50` then
         * `sub sp,sp,#0x240` -- and on parking the three arguments, but not
         * on where: 19A348 uses x22/x19/x20, 21A329 x25/x20/x22.  That is
         * what made the old anchor miss on 17 entirely, so every runtime
         * firmware download went on failing its trust evaluation with the
         * patch silently finding nothing.
         *
         * The frame alone is not an anchor -- seventeen sites in 21A329's
         * kernelcache.  Frame plus either triple is exactly one site each,
         * measured with `byte_search` on both images: the 21A329 pattern
         * `FD 43 01 91 FF 03 09 D1 F9 03 02 AA F4 03 01 AA F6 03 00 AA`
         * matches once (0xfffffff028d42884, `img4_firmware_evaluate` at
         * 0xfffffff028d42868), and 19A348's triple matches **zero** times in
         * that image, so the two arms cannot collide.
         *
         * The frame size is read rather than named, because the **same
         * function is also inside TXM** and is compiled with a smaller one --
         * `sub sp, sp, #0x210` against the kernel's `#0x240`, everything else
         * identical, both builds 2804 bytes.  The epilogue's `add` is then
         * the prologue's `sub` with the subtract bit cleared, so one scan
         * patches libimg4 wherever it is linked.
         */
        frame = adt_rd32(p + 4);
        if (adt_rd32(p) != 0x910143fdu ||          /* add x29, sp, #0x50    */
            (frame & 0xffc003ffu) != 0xd10003ffu) {/* sub sp, sp, #imm12    */
            continue;
        }
        if (!(adt_rd32(p + 8) == 0xaa0203f6u &&    /* 19A348: mov x22, x2   */
              adt_rd32(p + 12) == 0xaa0103f3u &&   /*         mov x19, x1   */
              adt_rd32(p + 16) == 0xaa0003f4u) &&
            !(adt_rd32(p + 8) == 0xaa0203f9u &&    /* 21A329: mov x25, x2   */
              adt_rd32(p + 12) == 0xaa0103f4u &&   /*         mov x20, x1   */
              adt_rd32(p + 16) == 0xaa0003f6u)) {  /*         mov x22, x0   */
            continue;
        }
        if (adt_rd32(p - 28) != 0xd503237fu ||     /* pacibsp               */
            adt_rd32(p - 24) != 0xa9ba6ffcu) {     /* stp x28, x27, [sp,#-0x60]! */
            /*
             * Keep looking rather than giving up: with two prologues in the
             * test, a near-miss is no longer a reason to conclude the image
             * has no such function.  The breadcrumb still records that it
             * happened, and a later match overwrites it.
             */
            adt_wr32(mark, 0xf0c2);
            continue;
        }
        for (j = 0; j < 0x1200u && at + j + 12 <= size; j += 4) {
            if (adt_rd32(p + j) != 0xaa1503e0u ||       /* mov x0, x21      */
                /* the matching `add sp, sp, #imm12`: the same word as the
                 * prologue's `sub` with the subtract bit cleared */
                adt_rd32(p + j + 4) != (frame & ~0x40000000u) ||
                adt_rd32(p + j + 8) != 0xa9457bfdu) {   /* ldp x29, x30     */
                continue;
            }
            adt_wr32(p + j, 0xd2800000u);               /* mov x0, #0       */
            adt_clean(p + j, p + j + 4);
            adt_wr32(mark, 0xf0cf);
            adt_wr32(mark + 4, (uint32_t)(uint64_t)(p + j));
            return;
        }
        adt_wr32(mark, 0xf0c3);                         /* no epilogue      */
        return;
    }
    adt_wr32(mark, 0xf0c4);                             /* no match         */
}

HANDOFF static void kc_relax_variant_check(
    uint8_t *base,
    uint64_t size,
    uint8_t *mark,
    uint8_t *clock_mark,
    uint8_t *counts
)
{
    const unsigned rows = sizeof(kc_patches) / sizeof(kc_patches[0]);
    uint64_t at;

    if (base == 0 || size < 12) {
        return;
    }
    adt_wr32(counts, 0x4b430000u | rows);
    for (unsigned r = 0; r < rows; r++) {
        adt_wr32(counts + 4 + 4 * r, 0);
    }
    unsigned found = 0;
    unsigned i;

    unsigned ssbs = 0;
    unsigned clock = 0;

    for (at = 0; at + 12 <= size; at += 4) {
        /*
         * Every access to PSTATE.SSBS, whichever register it names.  This host
         * does not implement FEAT_SSBS -- the M4 dropped it, the A15 this
         * firmware is built for has it -- so the encoding is unallocated and
         * UNDEFINED at decode.  That is not something a hypervisor can catch:
         * the syndrome is EC 0, not a trapped MSR/MRS, and an undefined
         * instruction at EL1 goes to the guest's own vector.  Matching the
         * encoding rather than its surroundings is what makes this complete --
         * the first attempt pinned one call site by its neighbours and left the
         * others to be found one boot at a time.
         *
         * A read becomes a zero and a write disappears, which agree: the thread
         * records SSBS clear and nothing tries to put it back.
         */
        uint32_t w = adt_rd32(base + at);

        if ((w & KC_RT_MASK) == KC_SSBS_MRS) {
            adt_wr32(base + at, KC_MOV_XZR | (w & 0x1fu));
            adt_clean(base + at, base + at + 4);
            ssbs++;
            continue;
        }
        if ((w & KC_RT_MASK) == KC_SSBS_MSR) {
            adt_wr32(base + at, KC_NOP);
            adt_clean(base + at, base + at + 4);
            ssbs++;
            continue;
        }
        /*
         * XNU's clock, moved off the register that cannot be made native.
         *
         * `ml_get_hwclock` and `ml_get_timebase` read ACNTVCT_EL0
         * (s3_4_c15_c10_6), and so does every `mach_absolute_time`, every
         * scheduler decision, every lock timeout and every trace point --
         * measured at 267,000 a second on the installed chain, level with the
         * guarded bank and ahead of the guest's own timer registers.  Each one
         * is a full exit: HACR_EL2.TACNT is in the host kernel's *fixed*
         * capability set, so no VMM can ask for it to run natively, and the
         * library answers it from `mach_absolute_time()` in userspace.
         *
         * CNTVCT_EL0 is the same counter.  The kernel says so itself:
         * `ml_get_timebase` and `ml_get_speculative_timebase` are the same
         * eleven instructions with the same rtclock offset added, differing
         * only in which of the two registers they read -- and the speculative
         * one is what `sleh_fiq`, `cpu_signal_handler_internal`,
         * `timer_state_event` and `dtrace_probe` already use.  The value
         * agrees on both sides of the swap because the offset does: the
         * library answers ACNTVCT as `mach_absolute_time()` minus the vCPU's
         * `virtual_timer_offset`, which is the word `hv_vcpu_set_vtimer_offset`
         * puts in CNTVOFF_EL2 and therefore exactly what the hardware
         * subtracts for a native CNTVCT read.
         *
         * What is given up is the ordering: ACNTVCT is the self-synchronising
         * read and CNTVCT may be speculated.  The kernel already accepts that
         * in the hot paths listed above, and on this machine the ACNTVCT read
         * is a round trip through userspace, which is a far stronger barrier
         * than the one being removed -- so the swap relaxes an ordering the
         * guest never actually had.
         *
         * The counter reads native only if the virtual counter is untrapped:
         * CNTHCTL_EL2.EL1TVCT (bit 14) is set by default, which would move the
         * traps rather than remove them.  Clear it with `OHV_CNTHCTL=0x1a000`.
         */
        if ((w & KC_RT_MASK) == KC_ACNTVCT_MRS) {
            adt_wr32(base + at, KC_CNTVCT_MRS | (w & 0x1fu));
            adt_clean(base + at, base + at + 4);
            clock++;
            continue;
        }
        for (i = 0; i < sizeof(kc_patches) / sizeof(kc_patches[0]); i++) {
            if (adt_rd32(base + at) != kc_patches[i].word[0] ||
                adt_rd32(base + at + 4) != kc_patches[i].word[1] ||
                adt_rd32(base + at + 8) != kc_patches[i].word[2]) {
                continue;
            }
            adt_wr32(
                base + at + 4 * kc_patches[i].replace,
                kc_patches[i].with
            );
            adt_clean(base + at, base + at + 12);
            found |= 1u << i;
            adt_wr32(counts + 4 + 4 * i, adt_rd32(counts + 4 + 4 * i) + 1);
            break;
        }
    }
#ifndef SPTM_HANDOFF_HOSTED     /* EL1 only; the host test runs at EL0 */
    __asm__ volatile("ic ialluis" ::: "memory");
#endif
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    adt_wr32(mark, (ssbs << 16) | 0xa5d0u | found);
    adt_wr32(clock_mark, clock);
}
