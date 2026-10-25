# External contract

These are the interfaces a host (emulator, hypervisor, or USB client) shares
with the ROM. Nothing in the ROM can detect a host that disagrees with them;
most failures show up far away from the cause, so change both sides or
neither.

## Memory map and devices

| Address | What |
| --- | --- |
| `0x00100000..0x00180000` | ROM image (reset vector at the base) |
| `0x00170000` | `.handoff` section: `sptm_handoff_prepare`, the fixed target that the iBoot handoff patch branches to. The address is baked into the uploaded patch rows. |
| `0x00160000` | handoff scratch page (ROM aperture). `+0` holds `0xb007b007` once the first handoff has run; the rest carries breadcrumb words a host can read back for diagnosis. |
| `0x00161000` | `BOOT_ARGS_EXTRA`, host-supplied boot-args (below) |
| `0x001c0000` | stack borrowed by the handoff code |
| `0x00400000` | platform register block: board ID `+0x10`, chip revision `+0x14`, ECID `+0x18`/`+0x1c`, security domain `+0x2c`, boot configuration `+0x30`, boot options `+0x34`, entropy `+0x38`, progress words `+0x64 + 4n`, stage marker `+0x3ac`, variant string `+0x5c8`, chip ID `+0x6f0` |
| `0x00400300` | boot context the ROM publishes: `{status_flags, boot_config, board_id, security_epoch}` |
| `0x20010000` | early console data register |
| `0x20070000` | pvpanic |
| `0x30000000` | SimpleVirtio root block device (virtual firmware for normal boot) |
| `0x30010000` | SimpleVirtio utility block device |
| `0x30100000` | SimpleVirtio USB device |
| `0x30250000` | SEP mailbox |
| `0x70000000` | work RAM (see [Work RAM layout](internals.md#work-ram-layout)) |
| `0x71000000` | personalized IMG4 upload buffer (DFU) |
| `0x71200000` | patch store (below) |

The stage marker is written with ASCII tags; `HOFF` marks the point just
before handoff, which a host can use as a deterministic entropy epoch (for
example by reseeding the platform entropy source when it sees it).

## Patch store

The ROM does not carry iBoot patches. The host provides one table per stage
and the ROM applies it while loading or handing off to that stage:

- Base `PATCH_STORE_BASE = 0x71200000`, one slot per stage every `0x40000`
  bytes. Stage ordinals: `0` iBSS (also used for LLB on the normal-boot path),
  `1` iBEC, `2` SPTM, `3` kernel. A kernel-stage row whose offset has bit 31
  set patches TXM instead.
- Each slot is a 112-byte `AVPPatchStageHeader` (little-endian, packed) followed
  by `count` rows of `{uint32 offset; uint32 instruction}`:

  | Offset | Field |
  | --- | --- |
  | `0x00` | `magic` = `0x48435450` (`'PTCH'`) |
  | `0x04` | `version` = 1 |
  | `0x08` | `count` |
  | `0x0c` | `flags` (bit 0: identity fields are valid) |
  | `0x10` | `received` (bytes accepted, kept by the ROM) |
  | `0x14` | `next_chunk` (kept by the ROM) |
  | `0x18` | `compressed_size` |
  | `0x1c` | `raw_size` |
  | `0x20` | `load_base` (u64) |
  | `0x28` | `entry_exception_level` |
  | `0x2c` | reserved |
  | `0x30` | `compressed_digest` (SHA-256 of the IM4P payload) |
  | `0x50` | `raw_digest` (SHA-256 of the decompressed image) |

  With the identity flag set, the image is checked against these sizes and
  digests instead of a compiled-in description, so one ROM boots any build the
  host can describe.
- A slot counts only when `magic` and `version` match. `magic` must be
  written last, so a torn table never looks finished.
- A host can write the slots straight into guest memory before the first
  instruction runs. Alternatively they can be uploaded over USB, in DFU and in
  Recovery, with vendor requests on the default control pipe:

  | bmRequestType | bRequest | wValue | wIndex | Data | Action |
  | --- | --- | --- | --- | --- | --- |
  | `0x41` | `0x70` | stage | 0 | none | open: clear the header |
  | `0x41` | `0x71` | stage | chunk ordinal | blob bytes | append; ordinals must run 0, 1, 2, ... (`patchup.py` uses 0x400-byte chunks) |
  | `0x41` | `0x72` | stage | row count | none | seal: count must match the bytes received; writes `magic` |
  | `0xc0` | `0x73` | 0 | 0 | 48 bytes in | report: per stage `{count, received, sealed}` as three u32 |

  A refused request stalls. `tools/patchup.py` is the reference client.

When no table is present the ROM silently skips patching (the diagnostic
`iLLB patches applied` is simply missing), and an unpatched iBoot then fails
much later, typically in DRAM calibration.

## Host boot-args

At handoff the ROM appends host-supplied boot-args to the kernel command line,
so they can change without rebuilding the ROM. The host writes, at
`0x00161000`, the seven bytes `OMVBARG`, one mode byte (`R` replaces the whole
line iBoot built; anything else appends), then a NUL-terminated string. The
command line holds 512 bytes including what iBoot put there; the handoff
breadcrumb reports how many bytes were lost to truncation. The same blob
carries opt-in tokens that change the device-tree edits: `scaler0=1`,
`jpeg0=1`, `nodcp=1`, `noeapfs=1`,
`clpcnoane=1` (see `src/runtime/boot/sptm_handoff_tree.c`).

## USB identity

DFU enumerates as `05ac:` `0x1226 + (security_domain & 3)` (so `05ac:1227`
for security domain 1) and Recovery as `05ac:1281`. The serial string carries `CPID`, `BDID`, `ECID`,
`IBFL`, `SRNM`, `NONC` and `SNON` in the usual Apple format; `NONC` is the
first 32 bytes of SHA-384 over the cached eight-byte boot nonce and `SNON` the
little-endian bytes of the five SEP configuration words. The DFU endpoint is
compatible with pymobiledevice3's `IRecv`.

## Diagnostics

Console diagnostics are prefixed `OpenBootROM:` (the project's former name).
They are kept as they are so the ROM binary, and any host tooling that matches
on them, stay unchanged.
