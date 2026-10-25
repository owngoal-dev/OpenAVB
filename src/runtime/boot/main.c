#include "avp/devices/simplevirtio/simple_virtio.h"
#include "avp/devices/usb/usb_device.h"
#include "avp/image/boot/source_image.h"
#include "avp/image/boot/virtual_firmware.h"
#include "avp/platform/platform.h"
#include "avp/platform/platform_boot_policy_owner.h"
#include "avp/platform/platform_identity_status_nonce_owner.h"
#include "avp/protocol/sep/sep.h"
#include "avp/runtime/exception/pvpanic.h"
#include "avp/runtime/ipc/runtime_provider.h"
#include "avp/runtime/ipc/runtime_service.h"
#include "avp/runtime/memory/runtime_address_space.h"
#include "avp/runtime/memory/runtime_bootstrap_memory.h"
#include "avp/runtime/memory/runtime_mmu.h"
#include "avp/runtime/memory/runtime_physical_memory.h"
#include "avp/runtime/prng/runtime_prng.h"
#include "avp/runtime/support/runtime_compiler_outliners_recovered.h"
#include "avp/runtime/support/runtime_state.h"
#include "avp/runtime/support/stack_guard.h"
#include "avp/runtime/task/runtime_irq.h"
#include "avp/runtime/task/runtime_task.h"

enum {
  USB_STATE_IPA = 0x70005000,
  EARLY_CONSOLE_DATA_IPA = 0x20010000,
};

static void boot_diagnostic(const char *message) {
  volatile uint32_t *const data = (volatile uint32_t *)EARLY_CONSOLE_DATA_IPA;
  while (*message != '\0') {
    *data = (uint8_t)*message++;
  }
}

/* The reset owner crosses the bootstrap-to-runtime guard epoch in one frame. */
__attribute__((no_stack_protector)) void firmware_main(void) {
  platform_stage(0x53524331);
  platform_progress(0, 1);
  boot_diagnostic("OpenBootROM: [boot] runtime initialization\n");
  runtime_service_runtime_initialize();
  runtime_address_space_initialize();
  runtime_address_space_bind_page_table(runtime_mmu_page_table());
  runtime_physical_memory_registry_initialize_matching();
  runtime_bootstrap_memory_initialize_matching();
  if (!runtime_bootstrap_memory_is_boot_owner()) {
    __builtin_trap();
  }
  runtime_compiler_outliners_bind_fixed_state(
      (void *)runtime_state_get(),
      runtime_bootstrap_memory_state()
  );
  runtime_provider_registry_initialize();
  runtime_irq_system_initialize();
  runtime_prng_platform_initialize();
  VirtualFirmwareStorage firmware_storage;
  if (!virtual_firmware_storage_initialize(&firmware_storage)) {
    boot_diagnostic("OpenBootROM: [boot] storage initialization failed\n");
    pvpanic_trigger();
  }
  boot_diagnostic("OpenBootROM: [boot] runtime and storage ready\n");

  AVPPlatformIdentity identity;
  platform_read_identity(&identity);
  const AVPBootPolicy boot_policy = platform_claim_boot_policy();
  AVPBootContext boot_context =
      boot_context_create(
          (uint8_t)boot_policy.configuration,
          identity.board_id,
          identity.security_epoch
      );
  platform_publish_boot_context(&boot_context);
  (void)platform_get_boot_nonce();
  boot_context_mark_boot_nonce_valid(&boot_context);
  platform_publish_boot_context(&boot_context);
  /* Bootstrap memory initialization has already cleared this owner window;
   * the policy reset publishes its expected flags and bounds here. */
  if (platform_boot_policy_reset(false) != 0) {
    boot_diagnostic("OpenBootROM: [boot] policy reset failed\n");
    pvpanic_trigger();
  }
  AVPBootSourceCandidate boot_source;
  const int32_t boot_attempt =
      boot_policy.selection == AVP_BOOT_SELECTION_FORCE_DFU ? -1 : 0;
  if (!boot_source_next_candidate(&boot_context, boot_attempt, &boot_source)) {
    boot_diagnostic("OpenBootROM: [boot] no boot source candidate\n");
    pvpanic_trigger();
  }
  pvpanic_initialize();

  SEPBootstrapResult sep = sep_bootstrap();
  platform_identity_prepare_usb_nonces(&identity, sep.configuration_words);
  boot_diagnostic("OpenBootROM: SEP bootstrap complete\n");
  runtime_stage_stack_guard(runtime_generate_stack_guard_seed() &
                            RUNTIME_STACK_GUARD_MASK);
  runtime_install_stack_guard();
  platform_progress(1, 1);
  platform_progress(3, sep.configuration_words[0]);

  if (boot_source.source == AVP_BOOT_SOURCE_VIRTUAL_FIRMWARE) {
    boot_diagnostic("OpenBootROM: [boot] loading virtual firmware\n");
    uint32_t image_size = 0;
    if (virtual_firmware_load_from_storage(&firmware_storage, &image_size)) {
      boot_diagnostic("OpenBootROM: virtual firmware loaded\n");
    }
    if (image_size != 0 && source_image_prepare_virtual_firmware(image_size)) {
      boot_diagnostic("OpenBootROM: virtual firmware prepared\n");
      platform_boot_policy_apply_entropy_identity_state();
      if (platform_boot_policy_publish_entropy_request() != 0) {
        boot_diagnostic("OpenBootROM: [boot] entropy publication failed\n");
        pvpanic_trigger();
      }
      sep_finalize_bootstrap(&sep);
      platform_progress(0, 2);
      platform_progress(1, 0);
      platform_progress(2, 0);
      platform_progress(3, 0);
      boot_diagnostic("OpenBootROM: [handoff] entering installed firmware\n");
      source_image_handoff(
          source_image_base(),
          source_image_size(),
          source_image_entry(),
          SOURCE_IMAGE_BOOT_MODE_NORMAL
      );
    }
    /* A missing or invalid virtual-firmware source advances the expected
     * candidate iterator to DFU while preserving the live SEP session. */
    if (!boot_source_next_candidate(&boot_context, 1, &boot_source) ||
        boot_source.source != AVP_BOOT_SOURCE_DFU) {
      boot_diagnostic("OpenBootROM: [boot] DFU fallback unavailable\n");
      pvpanic_trigger();
    }
    boot_diagnostic(
        "OpenBootROM: [boot] virtual firmware unavailable; falling back to DFU\n");
  }
  platform_progress(0, 0x20001);

  SimpleVirtioUSB usb;
  simple_virtio_usb_initialize(&usb);
  platform_progress(2, 1);

  USBDevice *state = (USBDevice *)USB_STATE_IPA;
  usb_device_initialize(
      state,
      &identity,
      (uint8_t *)SOURCE_IMAGE_UPLOAD_BASE,
      SOURCE_IMAGE_CAPACITY,
      source_image_validate,
      USB_MODE_DFU
  );
  boot_diagnostic("OpenBootROM: [dfu] ready for image upload\n");
  for (;;) {
    const uint32_t event_size = simple_virtio_usb_wait_event(&usb);
    const USBEventResult result = usb_device_handle_event(
        state,
        usb.queue0_buffer,
        event_size,
        usb.queue1_buffer,
        SIMPLE_VIRTIO_QUEUE_CAPACITY
    );
    simple_virtio_usb_respond(&usb, result.response_size);
    if (result.action == USB_FIRMWARE_ACTION_REPOST_EVENT) {
      simple_virtio_usb_post_event(&usb);
    } else if (result.action == USB_FIRMWARE_ACTION_HANDOFF) {
      /* Both boot modes cross the same SEP readiness boundary. The
       * payload may issue runtime SEP commands immediately at entry. */
      boot_diagnostic("OpenBootROM: DFU image accepted\n");
      platform_boot_policy_apply_entropy_identity_state();
      if (platform_boot_policy_publish_entropy_request() != 0) {
        boot_diagnostic("OpenBootROM: [boot] entropy publication failed\n");
        pvpanic_trigger();
      }
      boot_diagnostic("OpenBootROM: DFU entropy published\n");
      sep_finalize_bootstrap(&sep);
      boot_diagnostic("OpenBootROM: DFU SEP finalized\n");
      /* An iBSS that is not told it came over USB takes itself for a local
       * boot, drops to the recovery prompt and panics there. */
      boot_context_mark_remote_boot(&boot_context);
      platform_publish_boot_context(&boot_context);
      simple_virtio_usb_shutdown();
      boot_diagnostic("OpenBootROM: [handoff] entering DFU image\n");
      source_image_handoff(
          source_image_base(),
          source_image_size(),
          source_image_entry(),
          SOURCE_IMAGE_BOOT_MODE_DFU
      );
    }
    platform_progress(3, platform_ecid() == identity.ecid ? 1 : 0);
  }
}
