LLVM_PREFIX ?= /opt/homebrew/opt/llvm/bin
CC := $(LLVM_PREFIX)/clang
HOST_CC ?= clang
LD := /opt/homebrew/bin/ld.lld
OBJCOPY := $(LLVM_PREFIX)/llvm-objcopy
READOBJ := $(LLVM_PREFIX)/llvm-readobj
R2 := r2
UV := uv run --no-project

BUILD := build
INCLUDES := -I include -I vendor -I vendor/freestanding -I vendor/lzfse \
	-I vendor/libzbitmap \
	-I vendor/tinyusb/src -I vendor/mbedtls/include \
	-I vendor/mbedtls/core -I vendor/mbedtls/library
HOST_INCLUDES := -I include -I vendor -I vendor/tinyusb/src \
	-I vendor/libzbitmap \
	-I vendor/mbedtls/include -I vendor/mbedtls/core \
	-I vendor/mbedtls/library
CFLAGS := --target=aarch64-none-elf -std=c17 -O2 -g \
	-ffreestanding -fno-builtin -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -fno-exceptions -mgeneral-regs-only \
	-mno-outline-atomics -mstrict-align -mbranch-protection=none -fno-pic \
	-fno-pie -fno-common -fno-vectorize -fno-slp-vectorize \
	-DNDEBUG -Wall -Wextra -Werror -Wno-sign-compare \
	-Wno-unused-but-set-global -Wno-unknown-warning-option -MMD -MP $(INCLUDES)
ROM_CFLAGS := $(CFLAGS) -fstack-protector-strong \
	-mstack-protector-guard=global -moutline
PAYLOAD_CFLAGS := $(CFLAGS) -fno-stack-protector
ASFLAGS := --target=aarch64-none-elf -g -c
LDFLAGS := -T linker.ld --gc-sections --no-undefined --build-id=none \
	--fatal-warnings -nostdlib -static

LZFSE_SOURCES := vendor/lzfse/lzfse_decode.c \
	vendor/lzfse/lzfse_decode_base.c vendor/lzfse/lzfse_fse.c \
	vendor/lzfse/lzvn_decode_base.c
LIBZBITMAP_SOURCES := vendor/libzbitmap/zbm_decompress.c
TINYUSB_SOURCES := vendor/tinyusb/src/tusb.c \
	vendor/tinyusb/src/common/tusb_fifo.c \
	vendor/tinyusb/src/device/usbd.c \
	vendor/tinyusb/src/class/dfu/dfu_device.c
MBEDTLS_SOURCES := vendor/mbedtls/library/hmac_drbg.c \
	vendor/mbedtls/library/md.c \
	vendor/mbedtls/library/memory_buffer_alloc.c \
	vendor/mbedtls/library/platform.c \
	vendor/mbedtls/library/platform_util.c \
	vendor/mbedtls/library/sha512.c
MBEDTLS_OBJECTS := $(patsubst vendor/%.c,$(BUILD)/vendor/%.o,\
	$(MBEDTLS_SOURCES))
rwildcard = $(foreach entry,$(wildcard $1*),\
	$(call rwildcard,$(entry)/,$2) $(filter $(subst *,%,$2),$(entry)))

C_SOURCES := $(sort $(call rwildcard,src/,*.c))
ASM_SOURCES := $(sort $(call rwildcard,src/,*.S))
C_SOURCE_NAMES := $(notdir $(C_SOURCES))
ASM_SOURCE_NAMES := $(notdir $(ASM_SOURCES))
ifneq ($(words $(C_SOURCE_NAMES)),$(words $(sort $(C_SOURCE_NAMES))))
$(error OpenAVB C source basenames must be unique)
endif
ifneq ($(words $(ASM_SOURCE_NAMES)),$(words $(sort $(ASM_SOURCE_NAMES))))
$(error OpenAVB assembly source basenames must be unique)
endif
C_SOURCE_DIRECTORIES := $(sort $(dir $(C_SOURCES)))
ASM_SOURCE_DIRECTORIES := $(sort $(dir $(ASM_SOURCES)))
vpath %.c $(C_SOURCE_DIRECTORIES)
vpath %.S $(ASM_SOURCE_DIRECTORIES)
C_OBJECTS := $(addprefix $(BUILD)/,$(sort $(C_SOURCE_NAMES:.c=.o))) \
	$(patsubst vendor/%.c,$(BUILD)/vendor/%.o,\
		$(LZFSE_SOURCES) $(LIBZBITMAP_SOURCES) $(TINYUSB_SOURCES) \
		$(MBEDTLS_SOURCES))
ASM_OBJECTS := $(addprefix $(BUILD)/,$(sort $(ASM_SOURCE_NAMES:.S=.o)))
OBJECTS := $(C_OBJECTS) $(ASM_OBJECTS)
-include $(C_OBJECTS:.o=.d) $(ASM_OBJECTS:.o=.d)
RUNTIME_EXCEPTION_OBJECTS := $(BUILD)/runtime.o \
	$(BUILD)/runtime_capability.o $(BUILD)/runtime_diagnostics.o \
	$(BUILD)/runtime_domain.o \
	$(BUILD)/runtime_event.o \
	$(BUILD)/runtime_event_arch.o \
	$(BUILD)/runtime_exception.o $(BUILD)/runtime_irq.o \
	$(BUILD)/runtime_irq_arch.o $(BUILD)/runtime_irq_system.o \
	$(BUILD)/runtime_provider.o \
	$(BUILD)/runtime_mmu.o $(BUILD)/runtime_page_table.o \
	$(BUILD)/runtime_page_table_pool.o \
	$(BUILD)/runtime_page_table_mapping.o \
	$(BUILD)/runtime_page_table_arch.o \
	$(BUILD)/platform_entropy.o $(BUILD)/runtime_prng.o \
	$(BUILD)/runtime_prng_platform.o $(MBEDTLS_OBJECTS) \
	$(BUILD)/runtime_service.o \
	$(BUILD)/runtime_service_arch.o $(BUILD)/runtime_svc.o \
	$(BUILD)/runtime_rpc_codec_recovered.o \
	$(BUILD)/runtime_state.o \
	$(BUILD)/runtime_task.o $(BUILD)/runtime_task_arch.o \
	$(BUILD)/runtime_timer.o $(BUILD)/runtime_timer_arch.o \
	$(BUILD)/runtime_exception_arch.o
ELF := $(BUILD)/OpenAVB.elf
ROM := $(BUILD)/OpenAVB.bin
IBSS_BUILD := $(BUILD)/ibss
IBSS_C_SOURCES := src/image/boot/boot_image.c src/platform/platform.c src/runtime/exception/pvpanic.c \
	src/runtime/support/runtime.c src/crypto/hash/sha256.c src/devices/simplevirtio/simple_virtio.c src/image/boot/source_image.c src/image/boot/patch_store.c \
	src/devices/simplevirtio/simple_virtio_dcd.c src/devices/usb/usb_device.c \
	src/devices/usb/usb_dfu_protocol_adapter.c src/devices/usb/usb_identity.c \
	$(LZFSE_SOURCES) $(TINYUSB_SOURCES) ibss/src/main.c
IBSS_ASM_SOURCES := ibss/src/start.S
IBSS_C_OBJECTS := $(patsubst %.c,$(IBSS_BUILD)/%.o,$(IBSS_C_SOURCES))
IBSS_ASM_OBJECTS := $(patsubst %.S,$(IBSS_BUILD)/%.o,$(IBSS_ASM_SOURCES))
IBSS_OBJECTS := $(IBSS_C_OBJECTS) $(IBSS_ASM_OBJECTS)
IBSS_ELF := $(BUILD)/SourceIBSS.elf
IBSS_IMAGE := $(BUILD)/SourceIBSS.bin
USB_IDENTITY_TEST := $(BUILD)/usb_identity_tests
PATCH_STORE_TEST := $(BUILD)/patch_store_tests
USB_EVENT_TEST := $(BUILD)/usb_event_tests
USB_DEVICE_TRANSCRIPT_TEST := $(BUILD)/usb_device_transcript_tests
USB_RECOVERY_TRANSCRIPT_TEST := $(BUILD)/usb_recovery_transcript_tests
USB_DFU_PROTOCOL_ADAPTER_TEST := \
	$(BUILD)/usb_dfu_protocol_adapter_tests
USB_RPC_TRANSPORT_ADAPTER_TEST := \
	$(BUILD)/usb_rpc_transport_adapter_tests
USB_RPC_COMMAND_OWNER_TEST := $(BUILD)/usb_rpc_command_owner_tests
USB_DESCRIPTOR_ADAPTER_TEST := $(BUILD)/usb_descriptor_adapter_tests
USB_DFU_IDENTITY_ADAPTER_TEST := $(BUILD)/usb_dfu_identity_adapter_tests
USB_DFU_STACK_LIFECYCLE_ADAPTER_TEST := \
	$(BUILD)/usb_dfu_stack_lifecycle_adapter_tests
USB_EP0_STATE_ADAPTER_TEST := $(BUILD)/usb_ep0_state_adapter_tests
USB_ENDPOINT_QUEUE_ADAPTER_TEST := \
	$(BUILD)/usb_endpoint_queue_adapter_tests
USB_TRANSFER_REQUEST_ADAPTER_TEST := \
	$(BUILD)/usb_transfer_request_adapter_tests
USB_TRANSFER_QUEUE_OWNER_TEST := $(BUILD)/usb_transfer_queue_owner_tests
USB_DEVICE_DISPATCH_ADAPTER_TEST := \
	$(BUILD)/usb_device_dispatch_adapter_tests
USB_DFU_DESCRIPTOR_OWNER_TEST := $(BUILD)/usb_dfu_descriptor_owner_tests
USB_DEVICE_CONTROLLER_OWNER_TEST := \
	$(BUILD)/usb_device_controller_owner_tests
USB_STACK_CLOSURE_OWNER_TEST := $(BUILD)/usb_stack_closure_owner_tests
DEVICE_REMAINING_CLOSURE_OWNER_TEST := \
	$(BUILD)/device_remaining_closure_owner_tests
BOOT_DEVICE_CLOSURE_OWNER_TEST := $(BUILD)/boot_device_closure_owner_tests
SEP_PROTOCOL_TEST := $(BUILD)/sep_protocol_tests
SEP_PROTOCOL_HELPER_ADAPTER_TEST := \
	$(BUILD)/sep_protocol_helper_adapter_tests
SEP_RUNTIME_TRANSPORT_TEST := $(BUILD)/sep_runtime_transport_tests
SEP_RUNTIME_TRANSPORT_LIFECYCLE_TEST := \
	$(BUILD)/sep_runtime_transport_lifecycle_tests
SEP_MAILBOX_CHANNEL_ADAPTER_TEST := \
	$(BUILD)/sep_mailbox_channel_adapter_tests
SEP_TRANSPORT_ROUTE_ADAPTER_TEST := \
	$(BUILD)/sep_transport_route_adapter_tests
BOOT_POLICY_TEST := $(BUILD)/boot_policy_tests
BOOT_PROGRESS_RECOVERED_TEST := $(BUILD)/boot_progress_recovered_tests
BOOT_RESET_RECOVERED_TEST := $(BUILD)/boot_reset_recovered_tests
BOOT_VIRTUAL_FIRMWARE_COMPAT_TEST := \
	$(BUILD)/boot_virtual_firmware_compat_tests
VIRTUAL_FIRMWARE_READER_TEST := $(BUILD)/virtual_firmware_reader_tests
BOOT_VIRTUAL_FIRMWARE_OUTLINERS_TEST := \
	$(BUILD)/boot_virtual_firmware_outliners_tests
BOOT_BREADCRUMBS_COMPAT_TEST := $(BUILD)/boot_breadcrumbs_compat_tests
BOOT_BREADCRUMB_OUTLINERS_TEST := $(BUILD)/boot_breadcrumb_outliners_tests
BOOT_IMAGE_TEST := $(BUILD)/boot_image_tests
BOOT_IMAGE_RECOVERED_HELPERS_TEST := \
	$(BUILD)/boot_image_recovered_helpers_tests
BOOT_IMAGE_OUTLINERS_TEST := $(BUILD)/boot_image_outliners_tests
BOOT_IMAGE_MEMORY_OUTLINERS_TEST := $(BUILD)/boot_image_memory_outliners_tests
BOOT_IMG4_PARSER_OWNER_TEST := $(BUILD)/boot_img4_parser_owner_tests
BOOT_IMG4_PROPERTY_VALIDATORS_TEST := \
	$(BUILD)/boot_img4_property_validators_tests
BOOT_IMG4_MISC_OWNER_TEST := $(BUILD)/boot_img4_misc_owner_tests
BOOT_PLATFORM_HOOKS_TEST := $(BUILD)/boot_platform_hooks_tests
BOOT_MMU_BOOTSTRAP_OWNER_TEST := $(BUILD)/boot_mmu_bootstrap_owner_tests
BOOT_FIRMWARE_PROVIDER_OWNER_TEST := \
	$(BUILD)/boot_firmware_provider_owner_tests
RUNTIME_BOOT_HANDOFF_OWNER_TEST := $(BUILD)/runtime_boot_handoff_owner_tests
SPTM_HANDOFF_KC_PATCHES_TEST := $(BUILD)/sptm_handoff_kc_patches_tests
SOURCE_IMAGE_HANDOFF_ABI_TEST := $(BUILD)/source_image_handoff_abi_tests
BOOT_ARGUMENTS_COMPAT_TEST := $(BUILD)/boot_arguments_compat_tests
BOOT_ARGUMENTS_OUTLINERS_TEST := $(BUILD)/boot_arguments_outliners_tests
SIMPLE_VIRTIO_BLOCK_TEST := $(BUILD)/simple_virtio_block_tests
SIMPLE_VIRTIO_USB_ASYNC_TEST := $(BUILD)/simple_virtio_usb_async_tests
SIMPLEVIRTIO_QUEUE_ADAPTER_TEST := \
	$(BUILD)/simplevirtio_queue_adapter_tests
SIMPLE_VIRTIO_STORAGE_ADAPTER_TEST := \
	$(BUILD)/simple_virtio_storage_adapter_tests
SIMPLE_VIRTIO_USB_ADAPTER_TEST := $(BUILD)/simple_virtio_usb_adapter_tests
PLATFORM_DMA_TEST := $(BUILD)/platform_dma_tests
PLATFORM_DMA_CACHE_OWNER_TEST := $(BUILD)/platform_dma_cache_owner_tests
PLATFORM_PANIC_OWNER_TEST := $(BUILD)/platform_panic_owner_tests
PLATFORM_IDENTITY_CONFIG_OWNER_TEST := \
	$(BUILD)/platform_identity_config_owner_tests
PLATFORM_IDENTITY_STATUS_NONCE_OWNER_TEST := \
	$(BUILD)/platform_identity_status_nonce_owner_tests
PLATFORM_DCC_IDENTITY_RECOVERED_TEST := \
	$(BUILD)/platform_dcc_identity_recovered_tests
PLATFORM_BOOT_POLICY_OWNER_TEST := $(BUILD)/platform_boot_policy_owner_tests
PLATFORM_ENTROPY_TRANSPORT_OWNER_TEST := \
	$(BUILD)/platform_entropy_transport_owner_tests
PLATFORM_ENTROPY_POLICY_OWNER_TEST := \
	$(BUILD)/platform_entropy_policy_owner_tests
PLATFORM_SYSCFG_OWNER_TEST := $(BUILD)/platform_syscfg_owner_tests
RUNTIME_REGISTRY_TEST := $(BUILD)/runtime_registry_tests
RUNTIME_REGISTRY_RECOVERED_TEST := $(BUILD)/runtime_registry_recovered_tests
RUNTIME_FIREBLOOM_LAYOUT_RECOVERED_TEST := \
	$(BUILD)/runtime_firebloom_layout_recovered_tests
RUNTIME_ADDRESS_SPACE_RECOVERED_TEST := \
	$(BUILD)/runtime_address_space_recovered_tests
RUNTIME_BOOTSTRAP_MEMORY_RECOVERED_TEST := \
	$(BUILD)/runtime_bootstrap_memory_recovered_tests
RUNTIME_SCHEDULER_RECOVERED_TEST := \
	$(BUILD)/runtime_scheduler_recovered_tests
RUNTIME_PRNG_BOOTSTRAP_DISPATCH_RECOVERED_TEST := \
	$(BUILD)/runtime_prng_bootstrap_dispatch_recovered_tests
RUNTIME_REMAINING_38_RECOVERED_TEST := \
	$(BUILD)/runtime_remaining_38_recovered_tests
RUNTIME_SVC_DISPATCH_RECOVERED_TEST := \
	$(BUILD)/runtime_svc_dispatch_recovered_tests
RUNTIME_PROVIDER_TEST := $(BUILD)/runtime_provider_tests
RUNTIME_PROVIDER_WINDOW_RECOVERED_TEST := \
	$(BUILD)/runtime_provider_window_recovered_tests
RUNTIME_PROVIDER_TYPED_RECOVERED_TEST := \
	$(BUILD)/runtime_provider_typed_recovered_tests
RUNTIME_CAPABILITY_TEST := $(BUILD)/runtime_capability_tests
RUNTIME_CAPABILITY_HELPERS_TEST := \
	$(BUILD)/runtime_capability_helpers_tests
RUNTIME_DIAGNOSTICS_TEST := $(BUILD)/runtime_diagnostics_tests
RUNTIME_TRACE_DIAGNOSTICS_TEST := \
	$(BUILD)/runtime_trace_diagnostics_tests
RUNTIME_DOMAIN_TEST := $(BUILD)/runtime_domain_tests
RUNTIME_DOMAIN_RECOVERED_TEST := $(BUILD)/runtime_domain_recovered_tests
RUNTIME_RPC_TEST := $(BUILD)/runtime_rpc_tests
RUNTIME_RPC_CODEC_RECOVERED_TEST := \
	$(BUILD)/runtime_rpc_codec_recovered_tests
RUNTIME_ROOT_RPC_COMMANDS_RECOVERED_TEST := \
	$(BUILD)/runtime_root_rpc_commands_recovered_tests
RUNTIME_SERVICE_TEST := $(BUILD)/runtime_service_tests
RUNTIME_SVC_TEST := $(BUILD)/runtime_svc_tests
RUNTIME_STATE_TEST := $(BUILD)/runtime_state_tests
RUNTIME_TASK_TEST := $(BUILD)/runtime_task_tests
RUNTIME_TASK_LIFECYCLE_TEST := $(BUILD)/runtime_task_lifecycle_tests
RUNTIME_TASK_LIFECYCLE_RECOVERED_TEST := \
	$(BUILD)/runtime_task_lifecycle_recovered_tests
RUNTIME_TASK_CONTEXT_RECOVERED_TEST := \
	$(BUILD)/runtime_task_context_recovered_tests
RUNTIME_TIMER_TEST := $(BUILD)/runtime_timer_tests
RUNTIME_TIME_RECOVERED_TEST := $(BUILD)/runtime_time_recovered_tests
RUNTIME_EVENT_TEST := $(BUILD)/runtime_event_tests
RUNTIME_IRQ_TEST := $(BUILD)/runtime_irq_tests
RUNTIME_IRQ_SYSTEM_TEST := $(BUILD)/runtime_irq_system_tests
RUNTIME_MEMORY_TEST := $(BUILD)/runtime_memory_tests
RUNTIME_MEMORY_RECOVERED_TEST := $(BUILD)/runtime_memory_recovered_tests
RUNTIME_MEMORY_POLICY_RECOVERED_TEST := \
	$(BUILD)/runtime_memory_policy_recovered_tests
RUNTIME_UNPRIVILEGED_MEMORY_RECOVERED_TEST := \
	$(BUILD)/runtime_unprivileged_memory_recovered_tests
RUNTIME_STRING_COMPAT_TEST := $(BUILD)/runtime_string_compat_tests
RUNTIME_BYTE_RING_TEST := $(BUILD)/runtime_byte_ring_tests
RUNTIME_BOUNDED_BITMAP_TEST := $(BUILD)/runtime_bounded_bitmap_tests
RUNTIME_COMPILER_OUTLINERS_RECOVERED_TEST := \
	$(BUILD)/runtime_compiler_outliners_recovered_tests
RUNTIME_EXCEPTION_FRAME_RECOVERED_TEST := \
	$(BUILD)/runtime_exception_frame_recovered_tests
RUNTIME_EXCEPTION_DISPATCH_RECOVERED_TEST := \
	$(BUILD)/runtime_exception_dispatch_recovered_tests
RUNTIME_CONSOLE_RECOVERED_TEST := $(BUILD)/runtime_console_recovered_tests
RUNTIME_FORMAT_COMPAT_TEST := $(BUILD)/runtime_format_compat_tests
RUNTIME_FORMAT_OUTLINERS_TEST := $(BUILD)/runtime_format_outliners_tests
RUNTIME_CORE_RECOVERED_TEST := $(BUILD)/runtime_core_recovered_tests
RUNTIME_CPU_RECOVERED_TEST := $(BUILD)/runtime_cpu_recovered_tests
RUNTIME_IRQ_RECOVERED_TEST := $(BUILD)/runtime_irq_recovered_tests
RUNTIME_EVENT_RECOVERED_TEST := $(BUILD)/runtime_event_recovered_tests
RUNTIME_BOOTSTRAP_MEMORY_TEST := $(BUILD)/runtime_bootstrap_memory_tests
RUNTIME_SUPPORT_TEST := $(BUILD)/runtime_support_tests
RUNTIME_ALLOCATOR_TEST := $(BUILD)/runtime_allocator_tests
RUNTIME_ALLOCATOR_ARRAY_TEST := $(BUILD)/runtime_allocator_array_tests
RUNTIME_ALLOCATOR_ROUTE_TUPLE_TEST := \
	$(BUILD)/runtime_allocator_route_tuple_tests
RUNTIME_ALLOCATOR_RECOVERED_TEST := \
	$(BUILD)/runtime_allocator_recovered_tests
RUNTIME_PHYSICAL_MEMORY_TEST := $(BUILD)/runtime_physical_memory_tests
RUNTIME_PHYSICAL_MEMORY_RECOVERED_TEST := \
	$(BUILD)/runtime_physical_memory_recovered_tests
RUNTIME_ADDRESS_SPACE_TEST := $(BUILD)/runtime_address_space_tests
RUNTIME_PAGE_TABLE_TEST := $(BUILD)/runtime_page_table_tests
RUNTIME_PAGE_TABLE_RECOVERED_TEST := \
	$(BUILD)/runtime_page_table_recovered_tests
RUNTIME_PAGE_ATTRIBUTES_RECOVERED_TEST := \
	$(BUILD)/runtime_page_attributes_recovered_tests
RUNTIME_FIREBLOOM_TEST := $(BUILD)/runtime_firebloom_tests
RUNTIME_FIREBLOOM_METADATA_TEST := \
	$(BUILD)/runtime_firebloom_metadata_tests
RUNTIME_FIREBLOOM_PANIC_RECOVERED_TEST := \
	$(BUILD)/runtime_firebloom_panic_recovered_tests
RUNTIME_FIREBLOOM_RELATIONS_RECOVERED_TEST := \
	$(BUILD)/runtime_firebloom_relations_recovered_tests
RUNTIME_FIREBLOOM_RANGES_RECOVERED_TEST := \
	$(BUILD)/runtime_firebloom_ranges_recovered_tests
RUNTIME_FIREBLOOM_TUPLES_RECOVERED_TEST := \
	$(BUILD)/runtime_firebloom_tuples_recovered_tests
STACK_GUARD_TEST := $(BUILD)/stack_guard_tests
RUNTIME_STACK_GUARD_HELPERS_TEST := \
	$(BUILD)/runtime_stack_guard_helpers_tests
PLATFORM_ENTROPY_TEST := $(BUILD)/platform_entropy_tests
RUNTIME_PRNG_TEST := $(BUILD)/runtime_prng_tests
RUNTIME_RECURSIVE_LOCK_TEST := $(BUILD)/runtime_recursive_lock_tests
RUNTIME_INTRUSIVE_QUEUE_TEST := $(BUILD)/runtime_intrusive_queue_tests
RUNTIME_QSORT_TEST := $(BUILD)/runtime_qsort_tests
APPLE_LIBDER_TEST := $(BUILD)/apple_libder_tests
IMG4_DECODE_COMPAT_TEST := $(BUILD)/img4_decode_compat_tests
IMG4_DECODE_EVALUATION_TEST := $(BUILD)/img4_decode_evaluation_tests
IMG4_CONTEXT_COMPAT_TEST := $(BUILD)/img4_context_compat_tests
IMG4_X509_COMPAT_TEST := $(BUILD)/img4_x509_compat_tests
IMG4_TRUST_COMPAT_TEST := $(BUILD)/img4_trust_compat_tests
IMG4_TRUST_EVALUATION_TEST := $(BUILD)/img4_trust_evaluation_tests
SYSCONFIG3_COMPAT_TEST := $(BUILD)/sysconfig3_compat_tests
APPLE_COMPRESSION_TEST := $(BUILD)/apple_compression_tests
APPLE_DEFLATE_LZ4_TEST := $(BUILD)/apple_deflate_lz4_tests
APPLE_LZBITMAP_TEST := $(BUILD)/apple_lzbitmap_tests
APPLE_LZSS_TEST := $(BUILD)/apple_lzss_tests
CORECRYPTO_MBEDTLS_TEST := $(BUILD)/corecrypto_mbedtls_tests
CORECRYPTO_RUNTIME_TEST := $(BUILD)/corecrypto_runtime_tests
CORECRYPTO_HMAC_DRBG_TEST := $(BUILD)/corecrypto_hmac_drbg_tests
CORECRYPTO_LIMB_COMPAT_TEST := $(BUILD)/corecrypto_limb_compat_tests
CORECRYPTO_RSA_PUBLIC_COMPAT_TEST := \
	$(BUILD)/corecrypto_rsa_public_compat_tests
CORECRYPTO_FIELD_COMPAT_TEST := $(BUILD)/corecrypto_field_compat_tests
CORECRYPTO_CCZP_ACCESSORS_TEST := $(BUILD)/corecrypto_cczp_accessors_tests
CORECRYPTO_ARITHMETIC_COMPAT_TEST := \
	$(BUILD)/corecrypto_arithmetic_compat_tests
CORECRYPTO_SHA512_COMPAT_TEST := $(BUILD)/corecrypto_sha512_compat_tests
CORECRYPTO_SHA3_COMPAT_TEST := $(BUILD)/corecrypto_sha3_compat_tests
FIREBLOOM_PROBE_ELF := $(BUILD)/OpenAVB.firebloom-probe.elf
FIREBLOOM_PROBE_ROM := $(BUILD)/OpenAVB.firebloom-probe.bin
PANIC_PROBE_ELF := $(BUILD)/OpenAVB.panic-probe.elf
PANIC_PROBE_ROM := $(BUILD)/OpenAVB.panic-probe.bin
BLOCK_PROBE_ELF := $(BUILD)/OpenAVB.block-probe.elf
BLOCK_PROBE_ROM := $(BUILD)/OpenAVB.block-probe.bin
PREFLIGHT_PROBE_ELF := $(BUILD)/OpenAVB.preflight-probe.elf
PREFLIGHT_PROBE_ROM := $(BUILD)/OpenAVB.preflight-probe.bin
BOOTSTRAP_PROBE_ELF := $(BUILD)/OpenAVB.bootstrap-probe.elf
BOOTSTRAP_PROBE_ROM := $(BUILD)/OpenAVB.bootstrap-probe.bin
RUNTIME_TRANSPORT_PROBE_ELF := $(BUILD)/OpenAVB.runtime-transport-probe.elf
RUNTIME_TRANSPORT_PROBE_ROM := $(BUILD)/OpenAVB.runtime-transport-probe.bin
RUNTIME_TRANSPORT_FULL_PROBE_ELF := \
	$(BUILD)/OpenAVB.runtime-transport-full-probe.elf
RUNTIME_TRANSPORT_FULL_PROBE_ROM := \
	$(BUILD)/OpenAVB.runtime-transport-full-probe.bin
RUNTIME_TRANSPORT_TIMEOUT_PROBE_ELF := \
	$(BUILD)/OpenAVB.runtime-transport-timeout-probe.elf
RUNTIME_TRANSPORT_TIMEOUT_PROBE_ROM := \
	$(BUILD)/OpenAVB.runtime-transport-timeout-probe.bin
STACK_PROTECTOR_PROBE_ELF := $(BUILD)/OpenAVB.stack-protector-probe.elf
STACK_PROTECTOR_PROBE_ROM := $(BUILD)/OpenAVB.stack-protector-probe.bin
RUNTIME_SVC_PROBE_ELF := $(BUILD)/OpenAVB.runtime-svc-probe.elf
RUNTIME_SVC_PROBE_ROM := $(BUILD)/OpenAVB.runtime-svc-probe.bin
RUNTIME_SVC_INVALID_PROBE_ELF := \
	$(BUILD)/OpenAVB.runtime-svc-invalid-probe.elf
RUNTIME_SVC_INVALID_PROBE_ROM := \
	$(BUILD)/OpenAVB.runtime-svc-invalid-probe.bin
RUNTIME_IPC_PROBE_ELF := $(BUILD)/OpenAVB.runtime-ipc-probe.elf
RUNTIME_IPC_PROBE_ROM := $(BUILD)/OpenAVB.runtime-ipc-probe.bin
RUNTIME_IPC_TOKEN_INVALID_PROBE_ELF := \
	$(BUILD)/OpenAVB.runtime-ipc-token-invalid-probe.elf
RUNTIME_IPC_TOKEN_INVALID_PROBE_ROM := \
	$(BUILD)/OpenAVB.runtime-ipc-token-invalid-probe.bin
RUNTIME_IPC_MAPPING_INVALID_PROBE_ELF := \
	$(BUILD)/OpenAVB.runtime-ipc-mapping-invalid-probe.elf
RUNTIME_IPC_MAPPING_INVALID_PROBE_ROM := \
	$(BUILD)/OpenAVB.runtime-ipc-mapping-invalid-probe.bin
SCHEDULER_PROBE_ELF := $(BUILD)/OpenAVB.scheduler-probe.elf
SCHEDULER_PROBE_ROM := $(BUILD)/OpenAVB.scheduler-probe.bin
TIMER_PROBE_ELF := $(BUILD)/OpenAVB.timer-probe.elf
TIMER_PROBE_ROM := $(BUILD)/OpenAVB.timer-probe.bin
EVENT_PROBE_ELF := $(BUILD)/OpenAVB.event-probe.elf
EVENT_PROBE_ROM := $(BUILD)/OpenAVB.event-probe.bin
EVENT_INVALID_PROBE_ELF := $(BUILD)/OpenAVB.event-invalid-probe.elf
EVENT_INVALID_PROBE_ROM := $(BUILD)/OpenAVB.event-invalid-probe.bin
NORMAL_BOOT_BUILD := $(BUILD)/normal-boot
NORMAL_BOOT_ELF := $(BUILD)/NormalBootPayload.elf
NORMAL_BOOT_IMAGE := $(BUILD)/NormalBootPayload.bin

.PHONY: all verify clean

all: $(ROM) $(IBSS_IMAGE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	mkdir -p $(@D)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(BUILD)/vendor/%.o: vendor/%.c | $(BUILD)
	mkdir -p $(@D)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(BUILD)/%.o: %.S | $(BUILD)
	mkdir -p $(@D)
	$(CC) $(ASFLAGS) $< -o $@

$(IBSS_BUILD)/%.o: %.c
	mkdir -p $(@D)
	$(CC) $(PAYLOAD_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(IBSS_BUILD)/%.o: %.S
	mkdir -p $(@D)
	$(CC) $(ASFLAGS) $< -o $@

$(ELF): $(OBJECTS) linker.ld
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@

$(ROM): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(IBSS_ELF): $(IBSS_OBJECTS) ibss/linker.ld
	$(LD) -T ibss/linker.ld --gc-sections --no-undefined --build-id=none \
		--fatal-warnings -nostdlib -static $(IBSS_OBJECTS) -o $@

$(IBSS_IMAGE): $(IBSS_ELF)
	$(OBJCOPY) -O binary $< $@
	$(UV) tools/finalize_source_image.py finalize $@

$(PATCH_STORE_TEST): tests/image/boot/patch_store_tests.c \
		src/image/boot/patch_store.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -DPATCH_STORE_HOSTED \
		$(HOST_INCLUDES) $^ -o $@

$(USB_IDENTITY_TEST): tests/devices/usb/usb_identity_tests.c src/devices/usb/usb_identity.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(USB_EVENT_TEST): tests/devices/usb/usb_event_tests.c src/devices/usb/usb_dfu.c \
	src/devices/usb/usb_identity.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(USB_DEVICE_TRANSCRIPT_TEST): tests/devices/usb/usb_device_transcript_tests.c \
		src/devices/usb/usb_device.c src/devices/usb/usb_dfu_protocol_adapter.c \
		src/devices/simplevirtio/simple_virtio_dcd.c src/devices/usb/usb_identity.c \
		src/image/boot/patch_store.c \
		$(TINYUSB_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -DPATCH_STORE_HOSTED \
		$(HOST_INCLUDES) -I vendor/tinyusb/src $^ -o $@

$(USB_RECOVERY_TRANSCRIPT_TEST): tests/devices/usb/usb_recovery_transcript_tests.c \
		src/devices/usb/usb_device.c src/devices/usb/usb_dfu_protocol_adapter.c \
		src/devices/simplevirtio/simple_virtio_dcd.c src/devices/usb/usb_identity.c \
		src/image/boot/patch_store.c \
		$(TINYUSB_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -DPATCH_STORE_HOSTED \
		$(HOST_INCLUDES) -I vendor/tinyusb/src $^ -o $@

$(USB_DFU_PROTOCOL_ADAPTER_TEST): \
		tests/devices/usb/usb_dfu_protocol_adapter_tests.c \
		src/devices/usb/usb_dfu_protocol_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_RPC_TRANSPORT_ADAPTER_TEST): \
		tests/devices/usb/usb_rpc_transport_adapter_tests.c \
		src/devices/usb/usb_rpc_transport_adapter.c src/runtime/ipc/rpc/runtime_rpc.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(USB_RPC_COMMAND_OWNER_TEST): tests/devices/usb/usb_rpc_command_owner_tests.c \
		src/devices/usb/usb_rpc_command_owner.c src/runtime/ipc/rpc/runtime_rpc.c src/runtime/support/runtime.c \
		src/runtime/exception/pvpanic.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(USB_DESCRIPTOR_ADAPTER_TEST): tests/devices/usb/usb_descriptor_adapter_tests.c \
		src/devices/usb/usb_descriptor_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_DFU_IDENTITY_ADAPTER_TEST): \
		tests/devices/usb/usb_dfu_identity_adapter_tests.c \
		src/devices/usb/usb_dfu_identity_adapter.c src/devices/usb/usb_identity.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_DFU_STACK_LIFECYCLE_ADAPTER_TEST): \
		tests/devices/usb/usb_dfu_stack_lifecycle_adapter_tests.c \
		src/devices/usb/usb_dfu_stack_lifecycle_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_EP0_STATE_ADAPTER_TEST): tests/devices/usb/usb_ep0_state_adapter_tests.c \
		src/devices/usb/usb_ep0_state_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_ENDPOINT_QUEUE_ADAPTER_TEST): \
		tests/devices/usb/usb_endpoint_queue_adapter_tests.c \
		src/devices/usb/usb_endpoint_queue_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_TRANSFER_REQUEST_ADAPTER_TEST): \
		tests/devices/usb/usb_transfer_request_adapter_tests.c \
		src/devices/usb/usb_transfer_request_adapter.c src/devices/usb/usb_descriptor_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_TRANSFER_QUEUE_OWNER_TEST): tests/devices/usb/usb_transfer_queue_owner_tests.c \
		src/devices/usb/usb_transfer_queue_owner.c src/devices/usb/usb_transfer_request_adapter.c \
		src/devices/usb/usb_descriptor_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_DEVICE_DISPATCH_ADAPTER_TEST): \
		tests/devices/usb/usb_device_dispatch_adapter_tests.c \
		src/devices/usb/usb_device_dispatch_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_DFU_DESCRIPTOR_OWNER_TEST): tests/devices/usb/usb_dfu_descriptor_owner_tests.c \
		src/devices/usb/usb_dfu_descriptor_owner.c src/devices/usb/usb_dfu_identity_adapter.c \
		src/devices/usb/usb_identity.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_DEVICE_CONTROLLER_OWNER_TEST): \
		tests/devices/usb/usb_device_controller_owner_tests.c \
		src/devices/usb/usb_device_controller_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(USB_STACK_CLOSURE_OWNER_TEST): tests/devices/usb/usb_stack_closure_owner_tests.c \
		src/devices/usb/usb_stack_closure_owner.c \
		src/devices/usb/usb_dfu_stack_lifecycle_adapter.c src/devices/usb/usb_ep0_state_adapter.c \
		src/devices/usb/usb_rpc_command_owner.c src/runtime/ipc/rpc/runtime_rpc.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(DEVICE_REMAINING_CLOSURE_OWNER_TEST): \
		tests/devices/device_remaining_closure_owner_tests.c \
		src/devices/device_remaining_closure_owner.c \
		src/devices/simplevirtio/simplevirtio_queue_adapter.c src/protocol/sep/sep_protocol_helper_adapter.c \
		src/protocol/sep/sep_protocol.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_PROGRESS_RECOVERED_TEST): tests/runtime/boot/boot_progress_recovered_tests.c \
		src/runtime/boot/boot_progress_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_RESET_RECOVERED_TEST): tests/runtime/boot/boot_reset_recovered_tests.c \
		src/runtime/boot/boot_reset_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_VIRTUAL_FIRMWARE_COMPAT_TEST): \
		tests/runtime/boot/boot_virtual_firmware_compat_tests.c \
		src/runtime/boot/boot_virtual_firmware_compat.c \
		src/crypto/hash/corecrypto_sha512_compat.c $(MBEDTLS_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(VIRTUAL_FIRMWARE_READER_TEST): \
		tests/image/boot/virtual_firmware_reader_tests.c \
		src/image/boot/virtual_firmware_reader.c \
		src/runtime/boot/boot_virtual_firmware_compat.c \
		src/crypto/hash/corecrypto_sha512_compat.c $(MBEDTLS_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_BREADCRUMBS_COMPAT_TEST): tests/runtime/boot/boot_breadcrumbs_compat_tests.c \
		src/runtime/boot/boot_breadcrumbs_compat.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_RECURSIVE_LOCK_TEST): \
		tests/runtime/support/runtime_recursive_lock_tests.c \
		src/runtime/support/runtime_recursive_lock.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_INTRUSIVE_QUEUE_TEST): tests/runtime/support/runtime_intrusive_queue_tests.c \
		src/runtime/support/runtime_intrusive_queue.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PAGE_ATTRIBUTES_RECOVERED_TEST): \
		tests/runtime/memory/runtime_page_attributes_recovered_tests.c \
		src/runtime/memory/runtime_bounded_bitmap.c \
		src/runtime/memory/page_attributes/runtime_page_attributes_recovered_registry.c \
		src/runtime/memory/page_attributes/runtime_page_attributes_recovered_mapping.c \
		src/runtime/memory/page_attributes/runtime_page_attributes_recovered_policy.c \
		src/runtime/memory/page_attributes/runtime_page_attributes_recovered_outliners.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(APPLE_LIBDER_TEST): tests/image/der/apple_libder_tests.c \
		src/image/der/apple_libder_decode.c src/image/der/apple_libder_sequence.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(IMG4_DECODE_COMPAT_TEST): tests/image/img4/img4_decode_compat_tests.c \
		src/image/img4/img4_decode_compat.c src/image/der/apple_libder_decode.c \
		src/image/der/apple_libder_sequence.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(IMG4_DECODE_EVALUATION_TEST): tests/image/img4/img4_decode_evaluation_tests.c \
		src/image/img4/img4_decode_hash.c src/image/img4/img4_decode_evaluation.c \
		src/image/img4/img4_decode_outliners.c src/image/img4/img4_decode_compat.c \
		src/crypto/support/corecrypto_mbedtls.c \
		src/image/der/apple_libder_decode.c src/image/der/apple_libder_sequence.c \
		$(MBEDTLS_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(IMG4_CONTEXT_COMPAT_TEST): tests/image/img4/img4_context_compat_tests.c \
		src/image/img4/img4_context_compat.c src/image/img4/img4_decode_compat.c \
		src/image/der/apple_libder_decode.c src/image/der/apple_libder_sequence.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(IMG4_X509_COMPAT_TEST): tests/image/img4/img4_x509_compat_tests.c \
		src/image/img4/img4_x509_compat.c src/image/der/apple_libder_decode.c \
		src/image/der/apple_libder_sequence.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(IMG4_TRUST_COMPAT_TEST): tests/image/img4/img4_trust_compat_tests.c \
		src/image/img4/img4_trust_compat.c src/crypto/rsa/corecrypto_rsa_public_compat.c \
		src/image/img4/img4_x509_compat.c src/image/der/apple_libder_decode.c \
		src/image/der/apple_libder_sequence.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(IMG4_TRUST_EVALUATION_TEST): tests/image/img4/img4_trust_evaluation_tests.c \
		src/image/img4/img4_trust_chain.c src/image/img4/img4_trust_evaluation.c \
		src/image/img4/img4_trust_compat.c src/image/img4/img4_context_compat.c \
		src/image/img4/img4_decode_compat.c src/image/img4/img4_x509_compat.c \
		src/crypto/rsa/corecrypto_rsa_public_compat.c \
		src/image/der/apple_libder_decode.c src/image/der/apple_libder_sequence.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ $(shell pkg-config --cflags --libs openssl) -o $@

$(SYSCONFIG3_COMPAT_TEST): tests/platform/configuration/sysconfig3_compat_tests.c \
		src/platform/configuration/sysconfig3_model_compat.c src/platform/configuration/sysconfig3_der_compat.c \
		src/image/der/apple_libder_decode.c src/image/der/apple_libder_sequence.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(APPLE_COMPRESSION_TEST): tests/image/compression/apple_compression_tests.c \
		src/image/compression/apple_compression_adapters.c src/image/compression/apple_compression_v1.c \
		$(LZFSE_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		-Wno-sign-compare $(HOST_INCLUDES) -I vendor/lzfse $^ -o $@

$(SEP_PROTOCOL_TEST): tests/protocol/sep/sep_protocol_tests.c src/protocol/sep/sep_protocol.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(SEP_PROTOCOL_HELPER_ADAPTER_TEST): \
		tests/protocol/sep/sep_protocol_helper_adapter_tests.c \
		src/protocol/sep/sep_protocol_helper_adapter.c src/protocol/sep/sep_protocol.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(SEP_RUNTIME_TRANSPORT_TEST): tests/protocol/sep/sep_runtime_transport_tests.c \
		src/protocol/sep/sep_runtime_transport_state.c \
		src/protocol/sep/sep_runtime_transport_provider.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(SEP_RUNTIME_TRANSPORT_LIFECYCLE_TEST): \
		tests/protocol/sep/sep_runtime_transport_lifecycle_tests.c \
		src/protocol/sep/sep_runtime_transport_lifecycle.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(SEP_MAILBOX_CHANNEL_ADAPTER_TEST): \
		tests/protocol/sep/sep_mailbox_channel_adapter_tests.c \
		src/protocol/sep/sep_mailbox_channel_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(SEP_TRANSPORT_ROUTE_ADAPTER_TEST): \
		tests/protocol/sep/sep_transport_route_adapter_tests.c \
		src/protocol/sep/sep_transport_route_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_POLICY_TEST): tests/runtime/boot/boot_policy_tests.c src/runtime/boot/boot_policy.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(BOOT_DEVICE_CLOSURE_OWNER_TEST): \
		tests/runtime/boot/boot_device_closure_owner_tests.c \
		src/runtime/boot/boot_device_closure_owner.c \
		src/devices/usb/usb_dfu_stack_lifecycle_adapter.c \
		src/devices/usb/usb_ep0_state_adapter.c \
		src/protocol/sep/sep_protocol_helper_adapter.c src/protocol/sep/sep_protocol.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_IMAGE_TEST): tests/image/boot/boot_image_tests.c src/image/boot/boot_image.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(BOOT_IMAGE_RECOVERED_HELPERS_TEST): \
		tests/image/boot/boot_image_recovered_helpers_tests.c \
		src/image/boot/boot_image_recovered_helpers.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_IMAGE_OUTLINERS_TEST): tests/image/boot/boot_image_outliners_tests.c \
		src/image/boot/boot_image_outliners.c src/image/boot/boot_image_recovered_helpers.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_IMAGE_MEMORY_OUTLINERS_TEST): \
		tests/image/boot/boot_image_memory_outliners_tests.c \
		src/image/boot/boot_image_memory_outliners.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_IMG4_PARSER_OWNER_TEST): tests/image/boot/boot_img4_parser_owner_tests.c \
		src/image/boot/boot_img4_parser_owner.c src/image/der/apple_libder_decode.c \
		src/image/der/apple_libder_sequence.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_IMG4_PROPERTY_VALIDATORS_TEST): \
		tests/image/boot/boot_img4_property_validators_tests.c \
		src/image/boot/boot_img4_property_validators.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_IMG4_MISC_OWNER_TEST): tests/image/boot/boot_img4_misc_owner_tests.c \
		src/image/boot/boot_img4_misc_owner.c src/image/der/apple_libder_decode.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_PLATFORM_HOOKS_TEST): tests/runtime/boot/boot_platform_hooks_tests.c \
		src/runtime/boot/boot_platform_hooks.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_MMU_BOOTSTRAP_OWNER_TEST): tests/runtime/boot/boot_mmu_bootstrap_owner_tests.c \
		src/runtime/boot/boot_mmu_bootstrap_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_ARGUMENTS_COMPAT_TEST): tests/runtime/boot/boot_arguments_compat_tests.c \
		src/runtime/boot/boot_arguments_compat.c src/runtime/memory/runtime_memory_recovered_strings.c \
		src/runtime/memory/runtime_memory_recovered_secure.c src/runtime/support/runtime.c \
		src/runtime/firebloom/runtime_firebloom.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -fno-builtin -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_ARGUMENTS_OUTLINERS_TEST): tests/runtime/boot/boot_arguments_outliners_tests.c \
		src/runtime/boot/boot_arguments_outliners.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_VIRTUAL_FIRMWARE_OUTLINERS_TEST): \
		tests/runtime/boot/boot_virtual_firmware_outliners_tests.c \
		src/runtime/boot/boot_virtual_firmware_outliners.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_BREADCRUMB_OUTLINERS_TEST): \
		tests/runtime/boot/boot_breadcrumb_outliners_tests.c \
		src/runtime/boot/boot_breadcrumb_outliners.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(SIMPLE_VIRTIO_BLOCK_TEST): tests/devices/simplevirtio/simple_virtio_block_tests.c \
		src/devices/simplevirtio/simple_virtio.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(SIMPLE_VIRTIO_USB_ASYNC_TEST): tests/devices/simplevirtio/simple_virtio_usb_async_tests.c \
		src/devices/simplevirtio/simple_virtio_usb_async.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(SIMPLEVIRTIO_QUEUE_ADAPTER_TEST): \
		tests/devices/simplevirtio/simplevirtio_queue_adapter_tests.c \
		src/devices/simplevirtio/simplevirtio_queue_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(SIMPLE_VIRTIO_STORAGE_ADAPTER_TEST): \
		tests/devices/simplevirtio/simple_virtio_storage_adapter_tests.c \
		src/devices/simplevirtio/simple_virtio_storage_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(SIMPLE_VIRTIO_USB_ADAPTER_TEST): tests/devices/simplevirtio/simple_virtio_usb_adapter_tests.c \
		src/devices/simplevirtio/simple_virtio_usb_adapter.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(PLATFORM_DMA_TEST): tests/platform/platform_dma_tests.c src/platform/platform_dma.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(PLATFORM_DMA_CACHE_OWNER_TEST): tests/platform/platform_dma_cache_owner_tests.c \
		src/platform/platform_dma_cache_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(PLATFORM_PANIC_OWNER_TEST): tests/platform/platform_panic_owner_tests.c \
		src/platform/platform_panic_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(PLATFORM_IDENTITY_CONFIG_OWNER_TEST): \
		tests/platform/platform_identity_config_owner_tests.c \
		src/platform/platform_identity_config_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(PLATFORM_IDENTITY_STATUS_NONCE_OWNER_TEST): \
		tests/platform/platform_identity_status_nonce_owner_tests.c \
		src/platform/platform_identity_status_nonce_owner.c \
		src/crypto/hash/corecrypto_sha512_compat.c \
		$(MBEDTLS_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(PLATFORM_DCC_IDENTITY_RECOVERED_TEST): \
		tests/platform/platform_dcc_identity_recovered_tests.c \
		src/platform/platform_dcc_identity_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(PLATFORM_BOOT_POLICY_OWNER_TEST): \
		tests/platform/platform_boot_policy_owner_tests.c \
		src/platform/platform_boot_policy_owner.c \
		src/platform/platform_boot_policy_image_metadata.c \
		src/crypto/support/corecrypto_mbedtls.c $(MBEDTLS_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(PLATFORM_ENTROPY_TRANSPORT_OWNER_TEST): \
		tests/platform/platform_entropy_transport_owner_tests.c \
		src/platform/platform_entropy_transport_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(PLATFORM_ENTROPY_POLICY_OWNER_TEST): \
		tests/platform/platform_entropy_policy_owner_tests.c \
		src/platform/platform_entropy_policy_owner.c \
		src/platform/platform_entropy.c \
		src/platform/platform_entropy_transport_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(PLATFORM_SYSCFG_OWNER_TEST): \
		tests/platform/platform_syscfg_owner_tests.c \
		src/platform/platform_syscfg_owner.c \
		src/runtime/support/runtime_log.c \
		src/platform/configuration/sysconfig3_model_compat.c \
		src/platform/configuration/sysconfig3_der_compat.c \
		src/image/der/apple_libder_decode.c \
		src/image/der/apple_libder_sequence.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_BOOT_HANDOFF_OWNER_TEST): \
		tests/runtime/boot/runtime_boot_handoff_owner_tests.c \
		src/runtime/boot/runtime_boot_handoff_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

# The handoff's kc_patches matcher, run on the host: the test includes
# src/runtime/boot/sptm_handoff_tree.c itself (so the source is a prerequisite,
# not an input), and KC_IMAGE=<kernelcache> runs it over a real image.
$(SPTM_HANDOFF_KC_PATCHES_TEST): \
		tests/runtime/boot/sptm_handoff_kc_patches_tests.c \
		src/runtime/boot/sptm_handoff_tree.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $< -o $@

$(SOURCE_IMAGE_HANDOFF_ABI_TEST): \
		tests/runtime/boot/source_image_handoff_abi_tests.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(BOOT_FIRMWARE_PROVIDER_OWNER_TEST): \
		tests/runtime/boot/boot_firmware_provider_owner_tests.c \
		src/runtime/boot/boot_firmware_provider_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_REGISTRY_TEST): tests/runtime/ipc/runtime_registry_tests.c \
		src/runtime/ipc/runtime_registry.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_REGISTRY_RECOVERED_TEST): \
		tests/runtime/ipc/runtime_registry_recovered_tests.c \
		src/runtime/ipc/runtime_registry_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_FIREBLOOM_LAYOUT_RECOVERED_TEST): \
		tests/runtime/firebloom/runtime_firebloom_layout_recovered_tests.c \
		src/runtime/firebloom/runtime_firebloom_layout_recovered.c \
		src/runtime/firebloom/runtime_firebloom_layout_cache_recovered.c \
		src/runtime/firebloom/runtime_firebloom_relations_recovered.c \
		src/runtime/firebloom/runtime_firebloom_ranges_recovered_relations.c \
		src/runtime/firebloom/runtime_firebloom_ranges_recovered_type.c \
		src/runtime/firebloom/runtime_firebloom_ranges_recovered_span.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_ADDRESS_SPACE_RECOVERED_TEST): \
		tests/runtime/memory/runtime_address_space_recovered_tests.c \
		src/runtime/memory/address_space/runtime_address_space_recovered_profile.c \
		src/runtime/memory/address_space/runtime_address_space_recovered_mapping.c \
		src/runtime/memory/address_space/runtime_address_space_recovered_domain.c \
		src/runtime/memory/address_space/runtime_address_space.c src/runtime/ipc/runtime_capability.c \
		src/runtime/ipc/runtime_domain.c src/runtime/memory/runtime_physical_memory.c \
		src/runtime/firebloom/runtime_firebloom.c src/runtime/memory/page_table/runtime_page_table.c \
		src/runtime/memory/page_table/runtime_page_table_pool.c src/runtime/memory/page_table/runtime_page_table_mapping.c \
		src/runtime/support/runtime_state.c src/runtime/support/runtime.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_BOOTSTRAP_MEMORY_RECOVERED_TEST): \
		tests/runtime/memory/runtime_bootstrap_memory_recovered_tests.c \
		src/runtime/memory/bootstrap_memory/runtime_bootstrap_memory_state_recovered.c \
		src/runtime/memory/bootstrap_memory/runtime_bootstrap_memory_recovered_core.c \
		src/runtime/memory/bootstrap_memory/runtime_bootstrap_memory_recovered_publication.c \
		src/runtime/memory/bootstrap_memory/runtime_bootstrap_memory_recovered_activation.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_SCHEDULER_RECOVERED_TEST): \
		tests/runtime/task/runtime_scheduler_recovered_tests.c \
		src/runtime/task/runtime_scheduler_recovered_queue.c \
		src/runtime/task/runtime_scheduler_recovered_timer.c \
		src/runtime/task/runtime_scheduler_recovered_switch.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PRNG_BOOTSTRAP_DISPATCH_RECOVERED_TEST): \
		tests/runtime/prng/runtime_prng_bootstrap_dispatch_recovered_tests.c \
		src/runtime/prng/runtime_prng_recovered.c src/runtime/boot/runtime_bootstrap_recovered.c \
		src/runtime/boot/boot_policy_dispatch_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_REMAINING_38_RECOVERED_TEST): \
		tests/runtime/support/runtime_remaining_38_recovered_tests.c \
		src/image/boot/boot_image_sources_recovered.c src/image/boot/boot_image.c \
		src/image/img4/img4_context_compat.c src/image/img4/img4_decode_compat.c \
		src/image/der/apple_libder_decode.c src/image/der/apple_libder_sequence.c \
		src/runtime/ipc/runtime_endpoint_recovered.c \
		src/runtime/task/runtime_task_remaining_recovered.c \
		src/runtime/firebloom/runtime_firebloom_remaining_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_SVC_DISPATCH_RECOVERED_TEST): \
		tests/runtime/ipc/runtime_svc_dispatch_recovered_tests.c \
		src/runtime/ipc/svc/runtime_svc_dispatch_recovered_callbacks.c \
		src/runtime/ipc/svc/runtime_svc_dispatch_recovered_copy.c \
		src/runtime/ipc/svc/runtime_svc_dispatch_recovered_dispatch.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PROVIDER_TEST): tests/runtime/ipc/runtime_provider_tests.c \
		src/runtime/ipc/provider/runtime_provider.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PROVIDER_WINDOW_RECOVERED_TEST): \
		tests/runtime/ipc/runtime_provider_window_recovered_tests.c \
		src/runtime/ipc/provider/runtime_provider_window_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PROVIDER_TYPED_RECOVERED_TEST): \
		tests/runtime/ipc/runtime_provider_typed_recovered_tests.c \
		src/runtime/ipc/provider/runtime_provider.c \
		src/runtime/ipc/provider/runtime_provider_typed_recovered_descriptor.c \
		src/runtime/ipc/provider/runtime_provider_typed_recovered_storage.c \
		src/runtime/ipc/provider/runtime_provider_typed_recovered_support.c \
		src/runtime/ipc/provider/runtime_provider_typed_recovered_outliners.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_CAPABILITY_TEST): tests/runtime/ipc/runtime_capability_tests.c \
		src/runtime/ipc/runtime_capability.c src/runtime/support/runtime_state.c src/runtime/support/runtime.c \
		src/runtime/exception/pvpanic.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_DIAGNOSTICS_TEST): tests/runtime/support/runtime_diagnostics_tests.c \
		src/runtime/support/runtime_diagnostics.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_TRACE_DIAGNOSTICS_TEST): \
		tests/runtime/support/runtime_trace_diagnostics_tests.c src/runtime/support/runtime_trace.c \
		src/runtime/support/runtime_log.c src/runtime/exception/pvpanic.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_CAPABILITY_HELPERS_TEST): \
		tests/runtime/ipc/runtime_capability_helpers_tests.c \
		src/runtime/ipc/runtime_capability_helpers.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_RPC_TEST): tests/runtime/ipc/runtime_rpc_tests.c src/runtime/ipc/rpc/runtime_rpc.c \
		src/runtime/support/runtime.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_RPC_CODEC_RECOVERED_TEST): \
		tests/runtime/ipc/runtime_rpc_codec_recovered_tests.c \
		src/runtime/ipc/rpc/runtime_rpc_codec_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -fno-builtin -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_ROOT_RPC_COMMANDS_RECOVERED_TEST): \
		tests/runtime/ipc/runtime_root_rpc_commands_recovered_tests.c src/runtime/ipc/rpc/runtime_rpc.c \
		src/runtime/ipc/rpc/runtime_root_rpc_commands_recovered_commands.c \
		src/runtime/ipc/rpc/runtime_root_rpc_commands_recovered_fields.c \
		src/runtime/ipc/rpc/runtime_root_rpc_commands_recovered_frame.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_SERVICE_TEST): tests/runtime/ipc/runtime_service_tests.c \
		src/runtime/ipc/runtime_service.c src/runtime/task/runtime_task.c \
		src/runtime/ipc/runtime_capability.c src/runtime/ipc/runtime_domain.c src/runtime/task/runtime_event.c \
		src/runtime/ipc/rpc/runtime_rpc_codec_recovered.c src/runtime/ipc/svc/runtime_svc.c \
		src/runtime/support/runtime_state.c src/runtime/task/runtime_timer.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_SVC_TEST): tests/runtime/ipc/runtime_svc_tests.c src/runtime/ipc/svc/runtime_svc.c \
		src/runtime/ipc/runtime_domain.c src/runtime/ipc/runtime_capability.c \
		src/runtime/support/runtime_state.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_DOMAIN_TEST): tests/runtime/ipc/runtime_domain_tests.c src/runtime/ipc/runtime_domain.c \
		src/runtime/ipc/runtime_capability.c src/runtime/support/runtime_state.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_STATE_TEST): tests/runtime/support/runtime_state_tests.c src/runtime/support/runtime_state.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_TASK_TEST): tests/runtime/task/runtime_task_tests.c src/runtime/task/runtime_task.c \
		src/runtime/ipc/runtime_capability.c src/runtime/ipc/runtime_domain.c src/runtime/task/runtime_event.c \
		src/runtime/ipc/rpc/runtime_rpc_codec_recovered.c src/runtime/ipc/svc/runtime_svc.c \
		src/runtime/support/runtime_state.c src/runtime/task/runtime_timer.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_TASK_LIFECYCLE_TEST): tests/runtime/task/runtime_task_lifecycle_tests.c \
		src/runtime/task/runtime_task.c src/runtime/ipc/runtime_capability.c src/runtime/task/runtime_event.c \
		src/runtime/ipc/runtime_domain.c src/runtime/ipc/rpc/runtime_rpc_codec_recovered.c \
		src/runtime/ipc/svc/runtime_svc.c src/runtime/support/runtime_state.c \
		src/runtime/task/runtime_timer.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_TASK_LIFECYCLE_RECOVERED_TEST): \
		tests/runtime/task/runtime_task_lifecycle_recovered_tests.c \
		src/runtime/task/runtime_task_lifecycle_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_TIMER_TEST): tests/runtime/task/runtime_timer_tests.c src/runtime/task/runtime_timer.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_TIME_RECOVERED_TEST): tests/runtime/support/runtime_time_recovered_tests.c \
		src/runtime/support/runtime_time_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_QSORT_TEST): tests/runtime/support/runtime_qsort_tests.c src/runtime/support/runtime_qsort.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_EVENT_TEST): tests/runtime/task/runtime_event_tests.c src/runtime/task/runtime_task.c \
		src/runtime/ipc/runtime_capability.c src/runtime/ipc/runtime_domain.c src/runtime/task/runtime_event.c \
		src/runtime/ipc/rpc/runtime_rpc_codec_recovered.c src/runtime/ipc/svc/runtime_svc.c \
		src/runtime/support/runtime_state.c src/runtime/task/runtime_timer.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_IRQ_TEST): tests/runtime/task/runtime_irq_tests.c src/runtime/task/runtime_irq.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_IRQ_SYSTEM_TEST): tests/runtime/task/runtime_irq_system_tests.c \
		src/runtime/task/runtime_irq.c src/runtime/task/runtime_irq_system.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_MEMORY_TEST): tests/runtime/memory/runtime_memory_tests.c src/runtime/support/runtime.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_MEMORY_RECOVERED_TEST): tests/runtime/memory/runtime_memory_recovered_tests.c \
		src/runtime/memory/runtime_memory_recovered_strings.c \
		src/runtime/memory/runtime_memory_recovered_secure.c src/runtime/support/runtime.c \
		src/runtime/firebloom/runtime_firebloom.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -fno-builtin -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_MEMORY_POLICY_RECOVERED_TEST): \
		tests/runtime/memory/runtime_memory_policy_recovered_tests.c \
		src/runtime/memory/runtime_memory_policy_recovered_core.c \
		src/runtime/memory/runtime_memory_policy_recovered_bootstrap.c \
		src/runtime/memory/runtime_memory_policy_recovered_range.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_UNPRIVILEGED_MEMORY_RECOVERED_TEST): \
		tests/runtime/support/runtime_unprivileged_memory_recovered_tests.c \
		src/runtime/support/runtime_unprivileged_memory_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -fno-builtin -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_STRING_COMPAT_TEST): tests/runtime/support/runtime_string_compat_tests.c \
		src/runtime/support/runtime_string_compat.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_BYTE_RING_TEST): tests/runtime/support/runtime_byte_ring_tests.c \
		src/runtime/support/runtime_byte_ring.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_BOUNDED_BITMAP_TEST): tests/runtime/memory/runtime_bounded_bitmap_tests.c \
		src/runtime/memory/runtime_bounded_bitmap.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_BOOTSTRAP_MEMORY_TEST): tests/runtime/memory/runtime_bootstrap_memory_tests.c \
		src/runtime/memory/bootstrap_memory/runtime_bootstrap_memory.c src/runtime/boot/runtime_bootstrap_owner.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_SUPPORT_TEST): tests/runtime/support/runtime_support_tests.c \
		src/runtime/memory/allocator/runtime_allocator.c src/runtime/support/runtime_diagnostics.c \
		src/runtime/support/runtime_log.c src/runtime/support/runtime.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_ALLOCATOR_TEST): tests/runtime/memory/allocator/runtime_allocator_tests.c \
		src/runtime/memory/allocator/runtime_allocator.c src/runtime/support/runtime.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_ALLOCATOR_RECOVERED_TEST): \
		tests/runtime/memory/allocator/runtime_allocator_recovered_tests.c \
		src/runtime/memory/allocator/runtime_allocator_recovered_route.c \
		src/runtime/memory/allocator/runtime_allocator_recovered_extent.c \
		src/runtime/memory/allocator/runtime_allocator_recovered_support.c \
		src/runtime/memory/allocator/runtime_allocator.c src/runtime/memory/allocator/runtime_allocator_geometry.c \
		src/runtime/memory/allocator/runtime_allocator_route_tuple.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PHYSICAL_MEMORY_TEST): tests/runtime/memory/runtime_physical_memory_tests.c \
		src/runtime/memory/runtime_physical_memory.c src/runtime/firebloom/runtime_firebloom.c \
		src/runtime/support/runtime.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PHYSICAL_MEMORY_RECOVERED_TEST): \
		tests/runtime/memory/runtime_physical_memory_recovered_tests.c \
		src/runtime/memory/runtime_physical_memory_recovered.c \
		src/runtime/memory/runtime_physical_memory.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_ADDRESS_SPACE_TEST): tests/runtime/memory/runtime_address_space_tests.c \
		src/runtime/memory/address_space/runtime_address_space.c src/runtime/ipc/runtime_capability.c \
		src/runtime/ipc/runtime_domain.c src/runtime/memory/runtime_physical_memory.c \
		src/runtime/firebloom/runtime_firebloom.c src/runtime/memory/page_table/runtime_page_table.c \
		src/runtime/memory/page_table/runtime_page_table_pool.c src/runtime/memory/page_table/runtime_page_table_mapping.c \
		src/runtime/support/runtime_state.c src/runtime/support/runtime.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PAGE_TABLE_TEST): tests/runtime/memory/runtime_page_table_tests.c \
		src/runtime/memory/page_table/runtime_page_table.c src/runtime/memory/page_table/runtime_page_table_pool.c \
		src/runtime/memory/page_table/runtime_page_table_mapping.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PAGE_TABLE_RECOVERED_TEST): \
		tests/runtime/memory/runtime_page_table_recovered_tests.c \
		src/runtime/memory/page_table/runtime_page_table.c src/runtime/memory/page_table/runtime_page_table_pool.c \
		src/runtime/memory/page_table/runtime_page_table_mapping.c \
		src/runtime/memory/page_table/runtime_page_table_recovered_policy.c \
		src/runtime/memory/page_table/runtime_page_table_recovered_mapping.c \
		src/runtime/memory/page_table/runtime_page_table_recovered_bootstrap.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_ALLOCATOR_ARRAY_TEST): tests/runtime/memory/allocator/runtime_allocator_array_tests.c \
		src/runtime/memory/allocator/runtime_allocator_array.c src/runtime/memory/allocator/runtime_allocator_geometry.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_ALLOCATOR_ROUTE_TUPLE_TEST): \
		tests/runtime/memory/allocator/runtime_allocator_route_tuple_tests.c \
		src/runtime/memory/allocator/runtime_allocator_route_tuple.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_FIREBLOOM_TEST): tests/runtime/firebloom/runtime_firebloom_tests.c \
		src/runtime/firebloom/runtime_firebloom.c src/runtime/support/runtime.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_FIREBLOOM_METADATA_TEST): \
		tests/runtime/firebloom/runtime_firebloom_metadata_tests.c \
		src/runtime/firebloom/runtime_firebloom_metadata.c src/runtime/memory/allocator/runtime_allocator.c \
		src/runtime/firebloom/runtime_firebloom.c src/runtime/support/runtime.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror -fno-builtin \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_FIREBLOOM_PANIC_RECOVERED_TEST): \
		tests/runtime/firebloom/runtime_firebloom_panic_recovered_tests.c \
		src/runtime/firebloom/runtime_firebloom_panic_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_FIREBLOOM_RELATIONS_RECOVERED_TEST): \
		tests/runtime/firebloom/runtime_firebloom_relations_recovered_tests.c \
		src/runtime/firebloom/runtime_firebloom_relations_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_FIREBLOOM_RANGES_RECOVERED_TEST): \
		tests/runtime/firebloom/runtime_firebloom_ranges_recovered_tests.c \
		src/runtime/firebloom/runtime_firebloom_ranges_recovered_type.c \
		src/runtime/firebloom/runtime_firebloom_ranges_recovered_span.c \
		src/runtime/firebloom/runtime_firebloom_ranges_recovered_relations.c \
		src/runtime/firebloom/runtime_firebloom_relations_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_FIREBLOOM_TUPLES_RECOVERED_TEST): \
		tests/runtime/firebloom/runtime_firebloom_tuples_recovered_tests.c \
		src/runtime/firebloom/runtime_firebloom_tuples_recovered_constructors.c \
		src/runtime/firebloom/runtime_firebloom_tuples_recovered_outliners.c \
		src/runtime/firebloom/runtime_firebloom_tuples_recovered_validation.c \
		src/runtime/firebloom/runtime_firebloom_ranges_recovered_type.c \
		src/runtime/firebloom/runtime_firebloom_ranges_recovered_span.c \
		src/runtime/firebloom/runtime_firebloom_ranges_recovered_relations.c \
		src/runtime/firebloom/runtime_firebloom_relations_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(STACK_GUARD_TEST): tests/runtime/support/stack_guard_tests.c src/runtime/support/stack_guard/stack_guard.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) \
		$^ -o $@

$(RUNTIME_STACK_GUARD_HELPERS_TEST): \
		tests/runtime/support/runtime_stack_guard_helpers_tests.c \
		src/runtime/support/stack_guard/runtime_stack_guard_helpers.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_COMPILER_OUTLINERS_RECOVERED_TEST): \
		tests/runtime/support/runtime_compiler_outliners_recovered_tests.c \
		src/runtime/support/runtime_compiler_outliners_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_EXCEPTION_FRAME_RECOVERED_TEST): \
		tests/runtime/exception/runtime_exception_frame_recovered_tests.c \
		src/runtime/exception/runtime_exception_frame_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_EXCEPTION_DISPATCH_RECOVERED_TEST): \
		tests/runtime/exception/runtime_exception_dispatch_recovered_tests.c \
		src/runtime/exception/runtime_exception_dispatch_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_TASK_CONTEXT_RECOVERED_TEST): \
		tests/runtime/task/runtime_task_context_recovered_tests.c \
		src/runtime/task/runtime_task_context_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_CONSOLE_RECOVERED_TEST): tests/runtime/support/runtime_console_recovered_tests.c \
		src/runtime/support/runtime_console_recovered.c src/runtime/support/runtime_byte_ring.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_FORMAT_COMPAT_TEST): tests/runtime/support/runtime_format_compat_tests.c \
		src/runtime/support/runtime_format_compat.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_FORMAT_OUTLINERS_TEST): tests/runtime/support/runtime_format_outliners_tests.c \
		src/runtime/support/runtime_format_outliners.c src/runtime/support/runtime_format_compat.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_CORE_RECOVERED_TEST): tests/runtime/support/runtime_core_recovered_tests.c \
		src/runtime/support/runtime_core_recovered.c src/runtime/support/runtime_format_compat.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_CPU_RECOVERED_TEST): tests/runtime/support/runtime_cpu_recovered_tests.c \
		src/runtime/support/runtime_cpu_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_IRQ_RECOVERED_TEST): tests/runtime/task/runtime_irq_recovered_tests.c \
		src/runtime/task/runtime_irq_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_DOMAIN_RECOVERED_TEST): tests/runtime/ipc/runtime_domain_recovered_tests.c \
		src/runtime/ipc/runtime_domain_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(RUNTIME_EVENT_RECOVERED_TEST): tests/runtime/task/runtime_event_recovered_tests.c \
		src/runtime/task/runtime_event_recovered.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(APPLE_DEFLATE_LZ4_TEST): tests/image/compression/apple_deflate_lz4_tests.c \
		src/image/compression/apple_deflate.c src/image/compression/apple_lz4.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		-Wno-sign-compare $(HOST_INCLUDES) $^ -o $@

$(APPLE_LZBITMAP_TEST): tests/image/compression/apple_lzbitmap_tests.c \
		src/image/compression/apple_lzbitmap.c src/runtime/support/runtime_string_compat.c \
		$(LIBZBITMAP_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(APPLE_LZSS_TEST): tests/image/compression/apple_lzss_tests.c src/image/compression/apple_lzss.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(CORECRYPTO_MBEDTLS_TEST): tests/crypto/support/corecrypto_mbedtls_tests.c \
		src/crypto/support/corecrypto_mbedtls.c $(MBEDTLS_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(CORECRYPTO_RUNTIME_TEST): tests/crypto/support/corecrypto_runtime_tests.c \
		src/crypto/support/corecrypto_runtime.c vendor/mbedtls/library/platform.c \
		vendor/mbedtls/library/platform_util.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(CORECRYPTO_HMAC_DRBG_TEST): tests/crypto/drbg/corecrypto_hmac_drbg_tests.c \
		src/crypto/drbg/corecrypto_hmac_drbg.c src/crypto/support/corecrypto_mbedtls.c \
		$(MBEDTLS_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(CORECRYPTO_LIMB_COMPAT_TEST): tests/crypto/bignum/corecrypto_limb_compat_tests.c \
		src/crypto/bignum/corecrypto_limb_compat.c vendor/mbedtls/library/platform.c \
		vendor/mbedtls/library/platform_util.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ $(shell pkg-config --cflags --libs openssl) -o $@

$(CORECRYPTO_CCZP_ACCESSORS_TEST): \
		tests/crypto/bignum/corecrypto_cczp_accessors_tests.c \
		src/crypto/bignum/corecrypto_cczp_accessors.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(CORECRYPTO_RSA_PUBLIC_COMPAT_TEST): \
		tests/crypto/rsa/corecrypto_rsa_public_compat_tests.c \
		src/crypto/rsa/corecrypto_rsa_public_compat.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ -o $@

$(CORECRYPTO_FIELD_COMPAT_TEST): tests/crypto/bignum/corecrypto_field_compat_tests.c \
		src/crypto/bignum/corecrypto_field_basic.c src/crypto/bignum/corecrypto_field_montgomery.c \
		src/crypto/bignum/corecrypto_field_number_theory.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ $(shell pkg-config --cflags --libs openssl) -o $@

$(CORECRYPTO_ARITHMETIC_COMPAT_TEST): \
		tests/crypto/bignum/corecrypto_arithmetic_compat_tests.c \
		src/crypto/bignum/corecrypto_arithmetic_compat.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ $(shell pkg-config --cflags --libs openssl) -o $@

$(CORECRYPTO_SHA512_COMPAT_TEST): tests/crypto/hash/corecrypto_sha512_compat_tests.c \
		src/crypto/hash/corecrypto_sha512_compat.c $(MBEDTLS_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ $(shell pkg-config --cflags --libs openssl) -o $@

$(CORECRYPTO_SHA3_COMPAT_TEST): tests/crypto/hash/corecrypto_sha3_compat_tests.c \
		src/crypto/hash/corecrypto_sha3_compat.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror \
		$(HOST_INCLUDES) $^ $(shell pkg-config --cflags --libs openssl) -o $@

$(PLATFORM_ENTROPY_TEST): tests/platform/platform_entropy_tests.c \
		src/platform/platform_entropy.c | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(RUNTIME_PRNG_TEST): tests/runtime/prng/runtime_prng_tests.c src/runtime/prng/runtime_prng.c \
		src/platform/platform_entropy.c $(MBEDTLS_SOURCES) | $(BUILD)
	$(HOST_CC) -std=c17 -O2 -Wall -Wextra -Werror $(HOST_INCLUDES) $^ -o $@

$(BUILD)/firebloom_probe_main.o: tests/runtime/firebloom/firebloom_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(FIREBLOOM_PROBE_ELF): $(BUILD)/firebloom_probe_main.o \
		$(BUILD)/runtime.o $(BUILD)/pvpanic.o \
		$(RUNTIME_EXCEPTION_OBJECTS) $(BUILD)/stack_guard_arch.o \
		$(BUILD)/stack_protector.o $(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(FIREBLOOM_PROBE_ROM): $(FIREBLOOM_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/panic_probe_main.o: tests/runtime/exception/panic_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(PANIC_PROBE_ELF): $(BUILD)/panic_probe_main.o $(BUILD)/pvpanic.o \
	$(RUNTIME_EXCEPTION_OBJECTS) $(BUILD)/stack_guard_arch.o \
	$(BUILD)/stack_protector.o \
	$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(PANIC_PROBE_ROM): $(PANIC_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/block_probe_main.o: tests/image/boot/block_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(BLOCK_PROBE_ELF): $(BUILD)/block_probe_main.o $(BUILD)/platform.o \
		$(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/runtime_rpc.o $(BUILD)/runtime_service_arch.o \
		$(BUILD)/runtime_svc_arch.o $(BUILD)/simple_virtio.o \
		$(BUILD)/simple_virtio_domain.o \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(BLOCK_PROBE_ROM): $(BLOCK_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/preflight_probe_main.o: tests/protocol/sep/preflight_probe_main.c \
		tests/protocol/sep/preflight_probe.h | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -I tests -c $< -o $@

$(BUILD)/preflight_probe_registry.o: tests/protocol/sep/preflight_probe_registry.c \
		tests/protocol/sep/preflight_probe.h | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -I tests -c $< -o $@

$(PREFLIGHT_PROBE_ELF): $(BUILD)/preflight_probe_main.o \
		$(BUILD)/preflight_probe_registry.o $(BUILD)/mailbox_access.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o \
		$(RUNTIME_EXCEPTION_OBJECTS) $(BUILD)/sep.o $(BUILD)/sep_protocol.o \
		$(BUILD)/start.o \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/time.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(PREFLIGHT_PROBE_ROM): $(PREFLIGHT_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/bootstrap_probe_main.o: tests/runtime/boot/bootstrap_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(BOOTSTRAP_PROBE_ELF): $(BUILD)/bootstrap_probe_main.o \
		$(BUILD)/mailbox_access.o $(BUILD)/platform.o $(BUILD)/pvpanic.o \
		$(BUILD)/runtime_registry.o \
		$(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/sep.o \
		$(BUILD)/sep_protocol.o $(BUILD)/stack_guard_arch.o \
		$(BUILD)/stack_protector.o $(BUILD)/start.o $(BUILD)/time.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(BOOTSTRAP_PROBE_ROM): $(BOOTSTRAP_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/runtime_transport_probe_main.o: \
		tests/protocol/sep/runtime_transport_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(RUNTIME_TRANSPORT_PROBE_ELF): \
		$(BUILD)/runtime_transport_probe_main.o $(BUILD)/mailbox_access.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o \
		$(BUILD)/runtime_registry.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/sep.o \
		$(BUILD)/sep_protocol.o \
		$(BUILD)/sep_runtime_transport.o \
		$(BUILD)/sep_runtime_transport_provider.o \
		$(BUILD)/sep_runtime_transport_state.o $(BUILD)/stack_guard.o \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/runtime_profile.o $(BUILD)/start.o \
		$(BUILD)/time.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(RUNTIME_TRANSPORT_PROBE_ROM): $(RUNTIME_TRANSPORT_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/runtime_transport_full_probe_main.o: \
		tests/protocol/sep/runtime_transport_full_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(RUNTIME_TRANSPORT_FULL_PROBE_ELF): \
		$(BUILD)/runtime_transport_full_probe_main.o $(BUILD)/mailbox_access.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o \
		$(BUILD)/runtime_registry.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/sep.o \
		$(BUILD)/sep_protocol.o \
		$(BUILD)/sep_runtime_transport.o \
		$(BUILD)/sep_runtime_transport_provider.o \
		$(BUILD)/sep_runtime_transport_state.o $(BUILD)/stack_guard_arch.o \
		$(BUILD)/stack_protector.o $(BUILD)/start.o \
		$(BUILD)/time.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(RUNTIME_TRANSPORT_FULL_PROBE_ROM): $(RUNTIME_TRANSPORT_FULL_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/runtime_transport_timeout_probe_main.o: \
		tests/protocol/sep/runtime_transport_timeout_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(BUILD)/runtime_transport_timeout_probe_time.o: \
		tests/protocol/sep/runtime_transport_timeout_probe_time.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(RUNTIME_TRANSPORT_TIMEOUT_PROBE_ELF): \
		$(BUILD)/runtime_transport_timeout_probe_main.o \
		$(BUILD)/runtime_transport_timeout_probe_time.o \
		$(BUILD)/mailbox_access.o $(BUILD)/platform.o $(BUILD)/pvpanic.o \
		$(BUILD)/runtime_registry.o \
		$(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/sep.o \
		$(BUILD)/sep_protocol.o $(BUILD)/sep_runtime_transport.o \
		$(BUILD)/sep_runtime_transport_provider.o \
		$(BUILD)/sep_runtime_transport_state.o $(BUILD)/stack_guard_arch.o \
		$(BUILD)/stack_protector.o $(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(RUNTIME_TRANSPORT_TIMEOUT_PROBE_ROM): \
		$(RUNTIME_TRANSPORT_TIMEOUT_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/stack_protector_probe_main.o: \
		tests/runtime/support/stack_protector_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(STACK_PROTECTOR_PROBE_ELF): $(BUILD)/stack_protector_probe_main.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o $(BUILD)/stack_guard.o \
		$(RUNTIME_EXCEPTION_OBJECTS) $(BUILD)/stack_guard_arch.o \
		$(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(STACK_PROTECTOR_PROBE_ROM): $(STACK_PROTECTOR_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/runtime_svc_probe_main.o: tests/runtime/ipc/runtime_svc_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(RUNTIME_SVC_PROBE_ELF): $(BUILD)/runtime_svc_probe_main.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/runtime_svc_arch.o $(BUILD)/stack_guard_arch.o \
		$(BUILD)/stack_protector.o $(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(RUNTIME_SVC_PROBE_ROM): $(RUNTIME_SVC_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/runtime_svc_invalid_probe_main.o: \
		tests/runtime/ipc/runtime_svc_invalid_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(RUNTIME_SVC_INVALID_PROBE_ELF): \
		$(BUILD)/runtime_svc_invalid_probe_main.o $(BUILD)/platform.o \
		$(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(RUNTIME_SVC_INVALID_PROBE_ROM): $(RUNTIME_SVC_INVALID_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/runtime_ipc_queue_probe_main.o: \
		tests/runtime/ipc/runtime_ipc_queue_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(RUNTIME_IPC_PROBE_ELF): $(BUILD)/runtime_ipc_queue_probe_main.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(RUNTIME_IPC_PROBE_ROM): $(RUNTIME_IPC_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/runtime_ipc_token_invalid_probe_main.o: \
		tests/runtime/ipc/runtime_ipc_token_invalid_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(RUNTIME_IPC_TOKEN_INVALID_PROBE_ELF): \
		$(BUILD)/runtime_ipc_token_invalid_probe_main.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(RUNTIME_IPC_TOKEN_INVALID_PROBE_ROM): \
		$(RUNTIME_IPC_TOKEN_INVALID_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/runtime_ipc_mapping_invalid_probe_main.o: \
		tests/runtime/ipc/runtime_ipc_mapping_invalid_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(RUNTIME_IPC_MAPPING_INVALID_PROBE_ELF): \
		$(BUILD)/runtime_ipc_mapping_invalid_probe_main.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(RUNTIME_IPC_MAPPING_INVALID_PROBE_ROM): \
		$(RUNTIME_IPC_MAPPING_INVALID_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/scheduler_probe_main.o: tests/runtime/task/scheduler_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(SCHEDULER_PROBE_ELF): $(BUILD)/scheduler_probe_main.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(SCHEDULER_PROBE_ROM): $(SCHEDULER_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/timer_probe_main.o: tests/runtime/task/timer_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TIMER_PROBE_ELF): $(BUILD)/timer_probe_main.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(TIMER_PROBE_ROM): $(TIMER_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/event_probe_main.o: tests/runtime/task/event_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(EVENT_PROBE_ELF): $(BUILD)/event_probe_main.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(EVENT_PROBE_ROM): $(EVENT_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/event_invalid_probe_main.o: tests/runtime/task/event_invalid_probe_main.c | $(BUILD)
	$(CC) $(ROM_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(EVENT_INVALID_PROBE_ELF): $(BUILD)/event_invalid_probe_main.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o $(RUNTIME_EXCEPTION_OBJECTS) \
		$(BUILD)/stack_guard_arch.o $(BUILD)/stack_protector.o \
		$(BUILD)/start.o linker.ld
	$(LD) $(LDFLAGS) $(filter %.o,$^) -o $@

$(EVENT_INVALID_PROBE_ROM): $(EVENT_INVALID_PROBE_ELF)
	$(OBJCOPY) -O binary $< $@

$(NORMAL_BOOT_BUILD)/main.o: tests/image/boot/normal_boot_probe_main.c
	mkdir -p $(@D)
	$(CC) $(PAYLOAD_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(NORMAL_BOOT_BUILD)/start.o: ibss/src/start.S
	mkdir -p $(@D)
	$(CC) $(ASFLAGS) $< -o $@

$(NORMAL_BOOT_ELF): $(NORMAL_BOOT_BUILD)/main.o $(NORMAL_BOOT_BUILD)/start.o \
		$(BUILD)/platform.o $(BUILD)/pvpanic.o ibss/linker.ld
	$(LD) -T ibss/linker.ld --gc-sections --no-undefined --build-id=none \
		--fatal-warnings -nostdlib -static $(filter %.o,$^) -o $@

$(NORMAL_BOOT_IMAGE): $(NORMAL_BOOT_ELF)
	$(OBJCOPY) -O binary $< $@
	$(UV) tools/finalize_source_image.py finalize $@

verify: $(ROM) $(IBSS_IMAGE) $(PATCH_STORE_TEST) $(USB_IDENTITY_TEST) $(USB_EVENT_TEST) \
	$(USB_DEVICE_TRANSCRIPT_TEST) \
	$(USB_RECOVERY_TRANSCRIPT_TEST) \
	$(USB_DFU_PROTOCOL_ADAPTER_TEST) \
	$(USB_RPC_TRANSPORT_ADAPTER_TEST) \
	$(USB_RPC_COMMAND_OWNER_TEST) \
	$(USB_DESCRIPTOR_ADAPTER_TEST) \
	$(USB_DFU_IDENTITY_ADAPTER_TEST) \
	$(USB_DFU_STACK_LIFECYCLE_ADAPTER_TEST) \
	$(USB_EP0_STATE_ADAPTER_TEST) \
	$(USB_ENDPOINT_QUEUE_ADAPTER_TEST) \
	$(USB_TRANSFER_REQUEST_ADAPTER_TEST) \
	$(USB_TRANSFER_QUEUE_OWNER_TEST) \
	$(USB_DEVICE_DISPATCH_ADAPTER_TEST) \
	$(USB_DFU_DESCRIPTOR_OWNER_TEST) \
	$(USB_DEVICE_CONTROLLER_OWNER_TEST) \
	$(USB_STACK_CLOSURE_OWNER_TEST) \
	$(DEVICE_REMAINING_CLOSURE_OWNER_TEST) \
	$(BOOT_DEVICE_CLOSURE_OWNER_TEST) \
	$(SEP_PROTOCOL_TEST) $(SEP_PROTOCOL_HELPER_ADAPTER_TEST) \
	$(SEP_RUNTIME_TRANSPORT_TEST) \
	$(SEP_RUNTIME_TRANSPORT_LIFECYCLE_TEST) \
	$(SEP_MAILBOX_CHANNEL_ADAPTER_TEST) \
	$(SEP_TRANSPORT_ROUTE_ADAPTER_TEST) \
	$(BOOT_POLICY_TEST) $(BOOT_IMAGE_TEST) \
	$(BOOT_ARGUMENTS_COMPAT_TEST) \
	$(BOOT_IMAGE_RECOVERED_HELPERS_TEST) \
	$(BOOT_IMAGE_OUTLINERS_TEST) \
	$(BOOT_IMAGE_MEMORY_OUTLINERS_TEST) \
	$(BOOT_IMG4_PARSER_OWNER_TEST) \
	$(BOOT_IMG4_PROPERTY_VALIDATORS_TEST) \
	$(BOOT_IMG4_MISC_OWNER_TEST) \
	$(BOOT_PLATFORM_HOOKS_TEST) \
	$(BOOT_MMU_BOOTSTRAP_OWNER_TEST) \
	$(BOOT_ARGUMENTS_OUTLINERS_TEST) \
	$(BOOT_PROGRESS_RECOVERED_TEST) \
	$(BOOT_RESET_RECOVERED_TEST) \
	$(BOOT_VIRTUAL_FIRMWARE_COMPAT_TEST) \
	$(VIRTUAL_FIRMWARE_READER_TEST) \
	$(BOOT_VIRTUAL_FIRMWARE_OUTLINERS_TEST) \
	$(BOOT_BREADCRUMBS_COMPAT_TEST) \
	$(BOOT_BREADCRUMB_OUTLINERS_TEST) \
	$(FIREBLOOM_PROBE_ROM) $(PANIC_PROBE_ROM) \
	$(BLOCK_PROBE_ROM) $(PREFLIGHT_PROBE_ROM) $(BOOTSTRAP_PROBE_ROM) \
	$(RUNTIME_TRANSPORT_PROBE_ROM) \
	$(RUNTIME_TRANSPORT_FULL_PROBE_ROM) \
	$(RUNTIME_TRANSPORT_TIMEOUT_PROBE_ROM) \
	$(STACK_PROTECTOR_PROBE_ROM) \
	$(RUNTIME_SVC_PROBE_ROM) $(RUNTIME_SVC_INVALID_PROBE_ROM) \
	$(RUNTIME_IPC_PROBE_ROM) $(RUNTIME_IPC_TOKEN_INVALID_PROBE_ROM) \
	$(RUNTIME_IPC_MAPPING_INVALID_PROBE_ROM) \
	$(SCHEDULER_PROBE_ROM) $(TIMER_PROBE_ROM) $(EVENT_PROBE_ROM) \
	$(EVENT_INVALID_PROBE_ROM) \
	$(SIMPLE_VIRTIO_BLOCK_TEST) $(SIMPLE_VIRTIO_USB_ASYNC_TEST) \
	$(SIMPLEVIRTIO_QUEUE_ADAPTER_TEST) \
	$(SIMPLE_VIRTIO_STORAGE_ADAPTER_TEST) \
	$(SIMPLE_VIRTIO_USB_ADAPTER_TEST) \
	$(PLATFORM_DMA_TEST) \
	$(PLATFORM_DMA_CACHE_OWNER_TEST) \
	$(PLATFORM_PANIC_OWNER_TEST) \
	$(PLATFORM_IDENTITY_CONFIG_OWNER_TEST) \
	$(PLATFORM_IDENTITY_STATUS_NONCE_OWNER_TEST) \
	$(PLATFORM_DCC_IDENTITY_RECOVERED_TEST) \
	$(PLATFORM_BOOT_POLICY_OWNER_TEST) \
	$(PLATFORM_ENTROPY_TRANSPORT_OWNER_TEST) \
	$(PLATFORM_ENTROPY_POLICY_OWNER_TEST) \
	$(PLATFORM_SYSCFG_OWNER_TEST) \
	$(RUNTIME_BOOT_HANDOFF_OWNER_TEST) \
	$(SPTM_HANDOFF_KC_PATCHES_TEST) \
	$(SOURCE_IMAGE_HANDOFF_ABI_TEST) \
	$(BOOT_FIRMWARE_PROVIDER_OWNER_TEST) \
	$(RUNTIME_REGISTRY_TEST) $(RUNTIME_PROVIDER_TEST) \
	$(RUNTIME_REGISTRY_RECOVERED_TEST) \
	$(RUNTIME_FIREBLOOM_LAYOUT_RECOVERED_TEST) \
	$(RUNTIME_ADDRESS_SPACE_RECOVERED_TEST) \
	$(RUNTIME_BOOTSTRAP_MEMORY_RECOVERED_TEST) \
	$(RUNTIME_SCHEDULER_RECOVERED_TEST) \
	$(RUNTIME_PRNG_BOOTSTRAP_DISPATCH_RECOVERED_TEST) \
	$(RUNTIME_REMAINING_38_RECOVERED_TEST) \
	$(RUNTIME_SVC_DISPATCH_RECOVERED_TEST) \
	$(RUNTIME_PROVIDER_WINDOW_RECOVERED_TEST) \
	$(RUNTIME_PROVIDER_TYPED_RECOVERED_TEST) \
	$(RUNTIME_CAPABILITY_TEST) $(RUNTIME_DIAGNOSTICS_TEST) \
	$(RUNTIME_CAPABILITY_HELPERS_TEST) \
	$(RUNTIME_TRACE_DIAGNOSTICS_TEST) \
	$(RUNTIME_DOMAIN_TEST) \
	$(RUNTIME_DOMAIN_RECOVERED_TEST) \
	$(RUNTIME_RPC_TEST) $(RUNTIME_RPC_CODEC_RECOVERED_TEST) \
	$(RUNTIME_ROOT_RPC_COMMANDS_RECOVERED_TEST) \
	$(RUNTIME_SERVICE_TEST) \
	$(RUNTIME_SVC_TEST) $(RUNTIME_STATE_TEST) $(RUNTIME_TASK_TEST) \
	$(RUNTIME_TASK_LIFECYCLE_TEST) \
	$(RUNTIME_TASK_LIFECYCLE_RECOVERED_TEST) $(RUNTIME_TIMER_TEST) \
	$(RUNTIME_TASK_CONTEXT_RECOVERED_TEST) \
	$(RUNTIME_TIME_RECOVERED_TEST) \
	$(RUNTIME_EVENT_TEST) $(RUNTIME_IRQ_TEST) $(RUNTIME_IRQ_SYSTEM_TEST) \
	$(RUNTIME_MEMORY_TEST) $(RUNTIME_MEMORY_RECOVERED_TEST) \
	$(RUNTIME_MEMORY_POLICY_RECOVERED_TEST) \
	$(RUNTIME_UNPRIVILEGED_MEMORY_RECOVERED_TEST) \
	$(RUNTIME_STRING_COMPAT_TEST) \
	$(RUNTIME_BYTE_RING_TEST) $(RUNTIME_BOUNDED_BITMAP_TEST) \
	$(RUNTIME_COMPILER_OUTLINERS_RECOVERED_TEST) \
	$(RUNTIME_EXCEPTION_FRAME_RECOVERED_TEST) \
	$(RUNTIME_EXCEPTION_DISPATCH_RECOVERED_TEST) \
	$(RUNTIME_CONSOLE_RECOVERED_TEST) \
	$(RUNTIME_FORMAT_COMPAT_TEST) \
	$(RUNTIME_FORMAT_OUTLINERS_TEST) \
	$(RUNTIME_CORE_RECOVERED_TEST) \
	$(RUNTIME_CPU_RECOVERED_TEST) \
	$(RUNTIME_IRQ_RECOVERED_TEST) \
	$(RUNTIME_EVENT_RECOVERED_TEST) \
	$(RUNTIME_BOOTSTRAP_MEMORY_TEST) \
	$(RUNTIME_SUPPORT_TEST) $(RUNTIME_ALLOCATOR_TEST) \
	$(RUNTIME_ALLOCATOR_ARRAY_TEST) \
	$(RUNTIME_ALLOCATOR_ROUTE_TUPLE_TEST) \
	$(RUNTIME_ALLOCATOR_RECOVERED_TEST) \
	$(RUNTIME_PHYSICAL_MEMORY_TEST) $(RUNTIME_ADDRESS_SPACE_TEST) \
	$(RUNTIME_PHYSICAL_MEMORY_RECOVERED_TEST) \
	$(RUNTIME_PAGE_TABLE_TEST) \
	$(RUNTIME_PAGE_TABLE_RECOVERED_TEST) \
	$(RUNTIME_PAGE_ATTRIBUTES_RECOVERED_TEST) \
	$(RUNTIME_FIREBLOOM_TEST) $(RUNTIME_FIREBLOOM_METADATA_TEST) \
	$(RUNTIME_FIREBLOOM_PANIC_RECOVERED_TEST) \
	$(RUNTIME_FIREBLOOM_RELATIONS_RECOVERED_TEST) \
	$(RUNTIME_FIREBLOOM_RANGES_RECOVERED_TEST) \
	$(RUNTIME_FIREBLOOM_TUPLES_RECOVERED_TEST) \
	$(STACK_GUARD_TEST) $(RUNTIME_STACK_GUARD_HELPERS_TEST) \
	$(PLATFORM_ENTROPY_TEST) $(RUNTIME_PRNG_TEST) \
	$(RUNTIME_RECURSIVE_LOCK_TEST) $(APPLE_LIBDER_TEST) \
	$(IMG4_DECODE_COMPAT_TEST) \
	$(IMG4_DECODE_EVALUATION_TEST) \
	$(IMG4_CONTEXT_COMPAT_TEST) \
	$(IMG4_X509_COMPAT_TEST) \
	$(IMG4_TRUST_COMPAT_TEST) \
	$(IMG4_TRUST_EVALUATION_TEST) \
	$(SYSCONFIG3_COMPAT_TEST) \
	$(RUNTIME_INTRUSIVE_QUEUE_TEST) \
	$(RUNTIME_QSORT_TEST) \
	$(APPLE_COMPRESSION_TEST) \
	$(APPLE_DEFLATE_LZ4_TEST) \
	$(APPLE_LZBITMAP_TEST) \
	$(APPLE_LZSS_TEST) \
	$(CORECRYPTO_MBEDTLS_TEST) \
	$(CORECRYPTO_RUNTIME_TEST) \
	$(CORECRYPTO_HMAC_DRBG_TEST) \
	$(CORECRYPTO_LIMB_COMPAT_TEST) \
	$(CORECRYPTO_RSA_PUBLIC_COMPAT_TEST) \
	$(CORECRYPTO_FIELD_COMPAT_TEST) \
	$(CORECRYPTO_CCZP_ACCESSORS_TEST) \
	$(CORECRYPTO_ARITHMETIC_COMPAT_TEST) \
	$(CORECRYPTO_SHA512_COMPAT_TEST) \
	$(CORECRYPTO_SHA3_COMPAT_TEST) \
	$(NORMAL_BOOT_IMAGE)
	$(PATCH_STORE_TEST)
	$(UV) tools/check_patch_uploader_contract.py $(PATCH_STORE_TEST)
	$(UV) tools/gen_platform_profile.py --check
	$(USB_IDENTITY_TEST)
	$(USB_EVENT_TEST)
	$(USB_DEVICE_TRANSCRIPT_TEST)
	$(USB_RECOVERY_TRANSCRIPT_TEST)
	$(USB_DFU_PROTOCOL_ADAPTER_TEST)
	$(USB_RPC_TRANSPORT_ADAPTER_TEST)
	$(USB_RPC_COMMAND_OWNER_TEST)
	$(USB_DESCRIPTOR_ADAPTER_TEST)
	$(USB_DFU_IDENTITY_ADAPTER_TEST)
	$(USB_DFU_STACK_LIFECYCLE_ADAPTER_TEST)
	$(USB_EP0_STATE_ADAPTER_TEST)
	$(USB_ENDPOINT_QUEUE_ADAPTER_TEST)
	$(USB_TRANSFER_REQUEST_ADAPTER_TEST)
	$(USB_TRANSFER_QUEUE_OWNER_TEST)
	$(USB_DEVICE_DISPATCH_ADAPTER_TEST)
	$(USB_DFU_DESCRIPTOR_OWNER_TEST)
	$(USB_DEVICE_CONTROLLER_OWNER_TEST)
	$(USB_STACK_CLOSURE_OWNER_TEST)
	$(DEVICE_REMAINING_CLOSURE_OWNER_TEST)
	$(BOOT_DEVICE_CLOSURE_OWNER_TEST)
	$(BOOT_PROGRESS_RECOVERED_TEST)
	$(BOOT_RESET_RECOVERED_TEST)
	$(BOOT_VIRTUAL_FIRMWARE_COMPAT_TEST)
	$(VIRTUAL_FIRMWARE_READER_TEST)
	$(BOOT_VIRTUAL_FIRMWARE_OUTLINERS_TEST)
	$(BOOT_BREADCRUMBS_COMPAT_TEST)
	$(BOOT_BREADCRUMB_OUTLINERS_TEST)
	$(SEP_PROTOCOL_TEST)
	$(SEP_PROTOCOL_HELPER_ADAPTER_TEST)
	$(SEP_RUNTIME_TRANSPORT_TEST)
	$(SEP_RUNTIME_TRANSPORT_LIFECYCLE_TEST)
	$(SEP_MAILBOX_CHANNEL_ADAPTER_TEST)
	$(SEP_TRANSPORT_ROUTE_ADAPTER_TEST)
	$(BOOT_POLICY_TEST)
	$(BOOT_IMAGE_TEST)
	$(BOOT_IMAGE_RECOVERED_HELPERS_TEST)
	$(BOOT_IMAGE_OUTLINERS_TEST)
	$(BOOT_IMAGE_MEMORY_OUTLINERS_TEST)
	$(BOOT_IMG4_PARSER_OWNER_TEST)
	$(BOOT_IMG4_PROPERTY_VALIDATORS_TEST)
	$(BOOT_IMG4_MISC_OWNER_TEST)
	$(BOOT_PLATFORM_HOOKS_TEST)
	$(BOOT_MMU_BOOTSTRAP_OWNER_TEST)
	$(BOOT_ARGUMENTS_COMPAT_TEST)
	$(BOOT_ARGUMENTS_OUTLINERS_TEST)
	$(SIMPLE_VIRTIO_BLOCK_TEST)
	$(SIMPLE_VIRTIO_USB_ASYNC_TEST)
	$(SIMPLEVIRTIO_QUEUE_ADAPTER_TEST)
	$(SIMPLE_VIRTIO_STORAGE_ADAPTER_TEST)
	$(SIMPLE_VIRTIO_USB_ADAPTER_TEST)
	$(PLATFORM_DMA_TEST)
	$(PLATFORM_DMA_CACHE_OWNER_TEST)
	$(PLATFORM_PANIC_OWNER_TEST)
	$(PLATFORM_IDENTITY_CONFIG_OWNER_TEST)
	$(PLATFORM_IDENTITY_STATUS_NONCE_OWNER_TEST)
	$(PLATFORM_DCC_IDENTITY_RECOVERED_TEST)
	$(PLATFORM_BOOT_POLICY_OWNER_TEST)
	$(PLATFORM_ENTROPY_TRANSPORT_OWNER_TEST)
	$(PLATFORM_ENTROPY_POLICY_OWNER_TEST)
	$(PLATFORM_SYSCFG_OWNER_TEST)
	$(RUNTIME_BOOT_HANDOFF_OWNER_TEST)
	$(SPTM_HANDOFF_KC_PATCHES_TEST)
	$(SOURCE_IMAGE_HANDOFF_ABI_TEST)
	$(BOOT_FIRMWARE_PROVIDER_OWNER_TEST)
	$(RUNTIME_REGISTRY_TEST)
	$(RUNTIME_REGISTRY_RECOVERED_TEST)
	$(RUNTIME_FIREBLOOM_LAYOUT_RECOVERED_TEST)
	$(RUNTIME_ADDRESS_SPACE_RECOVERED_TEST)
	$(RUNTIME_BOOTSTRAP_MEMORY_RECOVERED_TEST)
	$(RUNTIME_SCHEDULER_RECOVERED_TEST)
	$(RUNTIME_PRNG_BOOTSTRAP_DISPATCH_RECOVERED_TEST)
	$(RUNTIME_REMAINING_38_RECOVERED_TEST)
	$(RUNTIME_SVC_DISPATCH_RECOVERED_TEST)
	$(RUNTIME_PROVIDER_TEST)
	$(RUNTIME_PROVIDER_WINDOW_RECOVERED_TEST)
	$(RUNTIME_PROVIDER_TYPED_RECOVERED_TEST)
	$(RUNTIME_CAPABILITY_TEST)
	$(RUNTIME_CAPABILITY_HELPERS_TEST)
	$(RUNTIME_DIAGNOSTICS_TEST)
	$(RUNTIME_TRACE_DIAGNOSTICS_TEST)
	$(RUNTIME_DOMAIN_TEST)
	$(RUNTIME_DOMAIN_RECOVERED_TEST)
	$(RUNTIME_RPC_TEST)
	$(RUNTIME_RPC_CODEC_RECOVERED_TEST)
	$(RUNTIME_ROOT_RPC_COMMANDS_RECOVERED_TEST)
	$(RUNTIME_SERVICE_TEST)
	$(RUNTIME_SVC_TEST)
	$(RUNTIME_STATE_TEST)
	$(RUNTIME_TASK_TEST)
	$(RUNTIME_TASK_LIFECYCLE_TEST)
	$(RUNTIME_TASK_LIFECYCLE_RECOVERED_TEST)
	$(RUNTIME_TASK_CONTEXT_RECOVERED_TEST)
	$(RUNTIME_TIMER_TEST)
	$(RUNTIME_TIME_RECOVERED_TEST)
	$(RUNTIME_EVENT_TEST)
	$(RUNTIME_IRQ_TEST)
	$(RUNTIME_IRQ_SYSTEM_TEST)
	$(RUNTIME_MEMORY_TEST)
	$(RUNTIME_MEMORY_RECOVERED_TEST)
	$(RUNTIME_MEMORY_POLICY_RECOVERED_TEST)
	$(RUNTIME_UNPRIVILEGED_MEMORY_RECOVERED_TEST)
	$(RUNTIME_STRING_COMPAT_TEST)
	$(RUNTIME_BYTE_RING_TEST)
	$(RUNTIME_BOUNDED_BITMAP_TEST)
	$(RUNTIME_BOOTSTRAP_MEMORY_TEST)
	$(RUNTIME_SUPPORT_TEST)
	$(RUNTIME_ALLOCATOR_TEST)
	$(RUNTIME_ALLOCATOR_ARRAY_TEST)
	$(RUNTIME_ALLOCATOR_ROUTE_TUPLE_TEST)
	$(RUNTIME_ALLOCATOR_RECOVERED_TEST)
	$(RUNTIME_PHYSICAL_MEMORY_TEST)
	$(RUNTIME_PHYSICAL_MEMORY_RECOVERED_TEST)
	$(RUNTIME_ADDRESS_SPACE_TEST)
	$(RUNTIME_PAGE_TABLE_TEST)
	$(RUNTIME_PAGE_TABLE_RECOVERED_TEST)
	$(RUNTIME_FIREBLOOM_TEST)
	$(RUNTIME_FIREBLOOM_METADATA_TEST)
	$(RUNTIME_FIREBLOOM_PANIC_RECOVERED_TEST)
	$(RUNTIME_FIREBLOOM_RELATIONS_RECOVERED_TEST)
	$(RUNTIME_FIREBLOOM_RANGES_RECOVERED_TEST)
	$(RUNTIME_FIREBLOOM_TUPLES_RECOVERED_TEST)
	$(STACK_GUARD_TEST)
	$(RUNTIME_STACK_GUARD_HELPERS_TEST)
	$(RUNTIME_COMPILER_OUTLINERS_RECOVERED_TEST)
	$(RUNTIME_EXCEPTION_FRAME_RECOVERED_TEST)
	$(RUNTIME_EXCEPTION_DISPATCH_RECOVERED_TEST)
	$(RUNTIME_CONSOLE_RECOVERED_TEST)
	$(RUNTIME_FORMAT_COMPAT_TEST)
	$(RUNTIME_FORMAT_OUTLINERS_TEST)
	$(RUNTIME_CORE_RECOVERED_TEST)
	$(RUNTIME_CPU_RECOVERED_TEST)
	$(RUNTIME_IRQ_RECOVERED_TEST)
	$(RUNTIME_EVENT_RECOVERED_TEST)
	$(RUNTIME_PAGE_ATTRIBUTES_RECOVERED_TEST)
	$(PLATFORM_ENTROPY_TEST)
	$(RUNTIME_PRNG_TEST)
	$(RUNTIME_RECURSIVE_LOCK_TEST)
	$(RUNTIME_INTRUSIVE_QUEUE_TEST)
	$(RUNTIME_QSORT_TEST)
	$(APPLE_LIBDER_TEST)
	$(IMG4_DECODE_COMPAT_TEST)
	$(IMG4_DECODE_EVALUATION_TEST)
	$(IMG4_CONTEXT_COMPAT_TEST)
	$(IMG4_X509_COMPAT_TEST)
	$(IMG4_TRUST_COMPAT_TEST)
	$(IMG4_TRUST_EVALUATION_TEST)
	$(SYSCONFIG3_COMPAT_TEST)
	$(APPLE_COMPRESSION_TEST)
	$(APPLE_DEFLATE_LZ4_TEST)
	$(APPLE_LZBITMAP_TEST)
	$(APPLE_LZSS_TEST)
	$(CORECRYPTO_MBEDTLS_TEST)
	$(CORECRYPTO_RUNTIME_TEST)
	$(CORECRYPTO_HMAC_DRBG_TEST)
	$(CORECRYPTO_LIMB_COMPAT_TEST)
	$(CORECRYPTO_RSA_PUBLIC_COMPAT_TEST)
	$(CORECRYPTO_FIELD_COMPAT_TEST)
	$(CORECRYPTO_CCZP_ACCESSORS_TEST)
	$(CORECRYPTO_ARITHMETIC_COMPAT_TEST)
	$(CORECRYPTO_SHA512_COMPAT_TEST)
	$(CORECRYPTO_SHA3_COMPAT_TEST)
	$(READOBJ) --file-headers --sections --symbols --relocations $(ELF) > $(BUILD)/readobj.txt
	rg -q "Format: elf64-littleaarch64" $(BUILD)/readobj.txt
	rg -q "Entry: 0x100000" $(BUILD)/readobj.txt
	rg -q "pvpanic_trigger" $(BUILD)/readobj.txt
	rg -q "sep_execute_runtime_transport_command" $(BUILD)/readobj.txt
	rg -q "sep_read_research_pattern_u64" $(BUILD)/readobj.txt
	rg -q "sep_read_research_pattern_word" $(BUILD)/readobj.txt
	rg -q "Name: runtime_transport_layout \(" $(BUILD)/readobj.txt
	rg -q "Name: runtime_transport_provider \(" $(BUILD)/readobj.txt
	rg -q "Name: runtime_transport_profile \(" $(BUILD)/readobj.txt
	rg -q "runtime_generate_stack_guard_seed" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_copy_span" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_fill_span" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_descriptor_instance_size" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_descriptor_element_shift" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_descriptor_type_identity" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_descriptor_instance_alignment" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_descriptor_variant_shift" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_allocate_typed_storage" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_restore_primary_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_restore_secondary_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_save_primary_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_promote_secondary_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_builder_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_builder_append_field" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_builder_finalize" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_field_find" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_field_next" $(BUILD)/readobj.txt
	rg -q "avp_boot_image_evaluate" $(BUILD)/readobj.txt
	rg -q "runtime_stage_stack_guard" $(BUILD)/readobj.txt
	rg -q "runtime_install_stack_guard" $(BUILD)/readobj.txt
	rg -q "runtime_initialize_read_only_thread_pointer" $(BUILD)/readobj.txt
	rg -q "runtime_domain_query_locality" $(BUILD)/readobj.txt
	rg -q "runtime_bootstrap_memory_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_memory_subtract_reserved_ranges" $(BUILD)/readobj.txt
	rg -q "runtime_bootstrap_memory_is_boot_owner" $(BUILD)/readobj.txt
	rg -q "runtime_allocator_copy_routes" $(BUILD)/readobj.txt
	rg -q "runtime_allocator_select_route" $(BUILD)/readobj.txt
	rg -q "runtime_allocator_select_available_route" $(BUILD)/readobj.txt
	rg -q "runtime_allocator_release_bounded" $(BUILD)/readobj.txt
	rg -q "runtime_allocator_release_address" $(BUILD)/readobj.txt
	rg -q "runtime_allocator_route_allocation_size" $(BUILD)/readobj.txt
	rg -q "runtime_allocator_route_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_lookup" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_zero" $(BUILD)/readobj.txt
	rg -q "runtime_address_space_mapping_publish" $(BUILD)/readobj.txt
	rg -q "runtime_address_space_mapping_resolve" $(BUILD)/readobj.txt
	rg -q "runtime_address_space_attach_region" $(BUILD)/readobj.txt
	rg -q "runtime_mmu_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_page_table_map" $(BUILD)/readobj.txt
	rg -q "runtime_page_table_unmap" $(BUILD)/readobj.txt
	rg -q "runtime_page_table_reprotect" $(BUILD)/readobj.txt
	rg -q "platform_entropy_reseed_from_source" $(BUILD)/readobj.txt
	rg -q "platform_entropy_initialize" $(BUILD)/readobj.txt
	rg -q "platform_entropy_seed_primary_data" $(BUILD)/readobj.txt
	rg -q "platform_entropy_enable_primary_request" $(BUILD)/readobj.txt
	rg -q "runtime_prng_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_prng_generate" $(BUILD)/readobj.txt
	rg -q "mbedtls_hmac_drbg_seed" $(BUILD)/readobj.txt
	rg -q "mbedtls_sha512_update" $(BUILD)/readobj.txt
	rg -q "usb_dfu_protocol_initialize" $(BUILD)/readobj.txt
	rg -q "usb_dfu_manifestation_complete" $(BUILD)/readobj.txt
	rg -q "runtime_recursive_lock_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_recursive_lock_acquire_with_timeout" $(BUILD)/readobj.txt
	rg -q "runtime_recursive_lock_release" $(BUILD)/readobj.txt
	rg -q "DERDecodeItemPartialBufferGetLength" $(BUILD)/readobj.txt
	rg -q "DERParseSequenceContent" $(BUILD)/readobj.txt
	rg -q "DEROidCompare" $(BUILD)/readobj.txt
	rg -q "_DERDecodeSysConfig3ParseAndPopulateMetadataSequence" $(BUILD)/readobj.txt
	rg -q "_allocAndCopyDERString" $(BUILD)/readobj.txt
	rg -q "DERDecodeSysConfig3" $(BUILD)/readobj.txt
	rg -q "_DERDecodeSysConfig3ParseSequence" $(BUILD)/readobj.txt
	rg -q "_DERDecodeSysConfig3ParseAndPopulatePayloadSequence" $(BUILD)/readobj.txt
	rg -q "_allocSysConfig3PayloadEntryInternal" $(BUILD)/readobj.txt
	rg -q "_allocAndCopyString" $(BUILD)/readobj.txt
	rg -q "_allocAndCopyData" $(BUILD)/readobj.txt
	rg -q "freeSysConfig3PayloadEntry" $(BUILD)/readobj.txt
	rg -q "sysConfig3PayloadEntryIsDeleted" $(BUILD)/readobj.txt
	rg -q "sysConfig3PayloadEntryGetString" $(BUILD)/readobj.txt
	rg -q "sysConfig3PayloadEntryGetData" $(BUILD)/readobj.txt
	rg -q "sysconfig3_payload_entry_get_data_invalid_arguments" $(BUILD)/readobj.txt
	rg -q "allocSysConfig3MetadataEntry" $(BUILD)/readobj.txt
	rg -q "freeSysConfig3MetadataEntry" $(BUILD)/readobj.txt
	rg -q "allocSysConfig3Manifest" $(BUILD)/readobj.txt
	rg -q "_copySysConfigData" $(BUILD)/readobj.txt
	rg -q "_allocSysConfig3ManifestFromDecodedSequence" $(BUILD)/readobj.txt
	rg -q "freeSysConfig3Manifest" $(BUILD)/readobj.txt
	rg -q "allocSysConfig3" $(BUILD)/readobj.txt
	rg -q "_sysConfig3SetManifest" $(BUILD)/readobj.txt
	rg -q "freeSysConfig3" $(BUILD)/readobj.txt
	rg -q "sysConfig3GetVersion" $(BUILD)/readobj.txt
	rg -q "sysConfig3GetPayloadEntryForKey" $(BUILD)/readobj.txt
	rg -q "_sysConfig3DeletePayloadEntryForKey" $(BUILD)/readobj.txt
	rg -q "_sysConfig3DeleteMetadataEntryForKey" $(BUILD)/readobj.txt
	rg -q "_sysConfig3AddPayloadEntry" $(BUILD)/readobj.txt
	rg -q "_sysConfig3AddMetadataEntry" $(BUILD)/readobj.txt
	rg -q "simplevirtio_device_initialize" $(BUILD)/readobj.txt
	rg -q "simplevirtio_commit_descriptor" $(BUILD)/readobj.txt
	rg -q "simplevirtio_device_deinitialize" $(BUILD)/readobj.txt
	rg -q "runtime_allocator_allocate_typed_array_aligned_impl" \
		$(BUILD)/readobj.txt
	rg -q "runtime_allocator_compute_array_sizes_checked" \
		$(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_element_geometry" $(BUILD)/readobj.txt
	rg -q "runtime_allocator_validate_route_tuple_tail_clone_4" \
		$(BUILD)/readobj.txt
	rg -q "runtime_allocator_outlined_load_record_route" \
		$(BUILD)/readobj.txt
	rg -q "lzfse_decode_lmd" $(BUILD)/readobj.txt
	rg -q "lzfse_decode_literals" $(BUILD)/readobj.txt
	rg -q "lzfse_decode_buffer" $(BUILD)/readobj.txt
	rg -q "compression_decode_buffer" $(BUILD)/readobj.txt
	rg -q "runtime_intrusive_queue_link_insert_after" $(BUILD)/readobj.txt
	rg -q "runtime_intrusive_queue_link_insert_before" $(BUILD)/readobj.txt
	rg -q "runtime_intrusive_queue_link_remove" $(BUILD)/readobj.txt
	rg -q "sep_runtime_transport_initialize" $(BUILD)/readobj.txt
	rg -q "sep_runtime_transport_teardown" $(BUILD)/readobj.txt
	rg -q "avp_virtual_counter_read_thunk" $(BUILD)/readobj.txt
	rg -q "runtime_conditional_delay_microseconds" $(BUILD)/readobj.txt
	rg -q "sep_mailbox_send_slot" $(BUILD)/readobj.txt
	rg -q "sep_mailbox_receive_slot" $(BUILD)/readobj.txt
	rg -q "sep_runtime_transport_channel_clear_route_state" \
		$(BUILD)/readobj.txt
	rg -q "runtime_qsort" $(BUILD)/readobj.txt
	rg -q "runtime_qsort_median_of_three" $(BUILD)/readobj.txt
	rg -q "runtime_compare_u64_ascending" $(BUILD)/readobj.txt
	rg -q "runtime_bounded_strlcat" $(BUILD)/readobj.txt
	rg -q "runtime_bounded_memmove" $(BUILD)/readobj.txt
	rg -q "runtime_byte_ring_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_byte_ring_write" $(BUILD)/readobj.txt
	rg -q "sep_interrupt_route_table_visit" $(BUILD)/readobj.txt
	rg -q "sep_runtime_transport_object_require" $(BUILD)/readobj.txt
	rg -q "lzbitmap_decode_rle" $(BUILD)/readobj.txt
	rg -q "lzbitmap_decode_body" $(BUILD)/readobj.txt
	rg -q "lzbitmap_decode_buffer" $(BUILD)/readobj.txt
	rg -q "runtime_lzss_decompress" $(BUILD)/readobj.txt
	rg -q "runtime_capability_allowed_external_audience_mask_span" \
		$(BUILD)/readobj.txt
	rg -q "runtime_object_address_is_admissible" $(BUILD)/readobj.txt
	rg -q "runtime_type6_resolve_wait_signal_capability_from_x23" \
		$(BUILD)/readobj.txt
	rg -q "cc_cmp_safe_internal" $(BUILD)/readobj.txt
	rg -q "ccdigest_internal" $(BUILD)/readobj.txt
	rg -q "ccdigest_update_internal" $(BUILD)/readobj.txt
	rg -q "cchmac_internal" $(BUILD)/readobj.txt
	rg -q "cchmac_final_internal" $(BUILD)/readobj.txt
	rg -q "cc_malloc_clear" $(BUILD)/readobj.txt
	rg -q "cc_ws_alloc" $(BUILD)/readobj.txt
	rg -q "cc_ws_free" $(BUILD)/readobj.txt
	rg -q "cc_try_abort" $(BUILD)/readobj.txt
	rg -q "cc_try_abort_if" $(BUILD)/readobj.txt
	rg -q "cc_try_abort_if_cold" $(BUILD)/readobj.txt
	rg -q "cc_clear" $(BUILD)/readobj.txt
	rg -q "cc_fault_canary_set" $(BUILD)/readobj.txt
	rg -q "cc_abort" $(BUILD)/readobj.txt
	rg -q "ccdrbg_init" $(BUILD)/readobj.txt
	rg -q "ccdrbg_reseed" $(BUILD)/readobj.txt
	rg -q "ccdrbg_generate" $(BUILD)/readobj.txt
	rg -q "ccdrbg_context_size" $(BUILD)/readobj.txt
	rg -q "ccdrbg_factory_nisthmac" $(BUILD)/readobj.txt
	rg -q "ccdrbg_nisthmac_done" $(BUILD)/readobj.txt
	rg -q "ccdrbg_nisthmac_must_reseed" $(BUILD)/readobj.txt
	rg -q "ccdrbg_nisthmac_update" $(BUILD)/readobj.txt
	rg -q "ccdrbg_nisthmac_init" $(BUILD)/readobj.txt
	rg -q "ccdrbg_nisthmac_generate" $(BUILD)/readobj.txt
	rg -q "ccdrbg_nisthmac_reseed" $(BUILD)/readobj.txt
	rg -q "sep_bootstrap_configuration_exchange" $(BUILD)/readobj.txt
	rg -q "sep_finalize_eight_round_handshake" $(BUILD)/readobj.txt
	rg -q "sep_validate_configuration_byte_span" $(BUILD)/readobj.txt
	rg -q "sep_clear_protocol_arguments" $(BUILD)/readobj.txt
	rg -q "sep_read_pattern_outlined_save_output_span" $(BUILD)/readobj.txt
	rg -q "sep_read_pattern_store_payload_success" $(BUILD)/readobj.txt
	rg -q "sep_build_protocol_message" $(BUILD)/readobj.txt
	rg -q "sep_wait_protocol_response" $(BUILD)/readobj.txt
	rg -q "sep_protocol_last_payload" $(BUILD)/readobj.txt
	rg -q "sep_protocol_last_opcode" $(BUILD)/readobj.txt
	rg -q "sep_protocol_expected_response_opcode" $(BUILD)/readobj.txt
	rg -q "runtime_bitmap_set_range" $(BUILD)/readobj.txt
	rg -q "runtime_bitmap_clear_range" $(BUILD)/readobj.txt
	rg -q "runtime_bitmap_classify_range" $(BUILD)/readobj.txt
	rg -q "runtime_bitmap_find_clear_index" $(BUILD)/readobj.txt
	rg -q "runtime_bitmap_find_clear_index_bounds_panic" $(BUILD)/readobj.txt
	rg -q "runtime_bitmap_source_token" $(BUILD)/readobj.txt
	rg -q "runtime_bitmap_find_clear_index_test_candidate_outlined" \
		$(BUILD)/readobj.txt
	rg -q "runtime_firebloom_store_u64" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_store_u64_bounds_panic" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_store_u64_pair" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_store_u64_pair_bounds_panic" \
		$(BUILD)/readobj.txt
	rg -q "runtime_firebloom_store_u64_triple" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_store_u64_triple_bounds_panic" \
		$(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_format" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_format_variadic" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_thunk_plain" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_thunk_spill_c" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_thunk_spill_d" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_thunk_spill_e" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_thunk_spill_f" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_thunk_spill_g" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_invoke_failure_hook_and_panic" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_callsite_offset" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_spill_scratch_registers" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_follow_identity_chain" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_evaluate_undirected" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_evaluate" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_resolve_and_cache" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_canonicalize_clone" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_compare_type_kind_to_indirect" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_save_type_pair_x20_x19" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_link_target" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_link_detach" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_index_set" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_index_store" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_slots_set" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_slots_store" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_merge_classes" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_state_require" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_state_access_panic" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_link_attach" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_metadata_store" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_load_type_relation_slots_to_x8" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_prepare_type_relation_metadata_layout" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_grow_capacity" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_set_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_set_destroy" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_canonicalize_operand" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_pair_hash" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_element_size" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_outlined_validate_saved_span_x22_x19" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_outlined_restore_span_bounds_x21_x19" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_outlined_save_span_bounds_x21_x19" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_outlined_validate_source_span_x23_x25" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_tuple_retype_slow" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_is_zero_sized" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_required_retype_failure" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_tuple_validate" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_element_size_clone" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_canonicalize" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_try_retype_span" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_prepare_invalid_span_candidate" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_aggregate_type_resolve_byte_offset" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_is_bytewise_clone" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_resolve" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_outlined_prepare_structural_retype_attempt" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_outlined_return_null_pointer" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_require_indirect_type_header" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_indirect_type_resolve_member" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_set_insert" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_type_relation_reduce" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_hash_u64" $(BUILD)/readobj.txt
	rg -q "simple_virtio_block_device_initialize" $(BUILD)/readobj.txt
	rg -q "simple_virtio_block_validate_device_tuple_thunk" \
		$(BUILD)/readobj.txt
	rg -q "simple_virtio_block_validate_device_tuple_thunk_clone" \
		$(BUILD)/readobj.txt
	rg -q "simple_virtio_block_outlined_device_span_x26" \
		$(BUILD)/readobj.txt
	rg -q "simple_virtio_block_validate_device_tuple" $(BUILD)/readobj.txt
	rg -q "simple_virtio_block_outlined_provider_span_x19_x28" \
		$(BUILD)/readobj.txt
	rg -q "simple_virtio_block_outlined_update_sector_state" \
		$(BUILD)/readobj.txt
	rg -q "simple_virtio_block_outlined_frame70_restore_x19_x28" \
		$(BUILD)/readobj.txt
	rg -q "simple_virtio_block_outlined_frame70_save_x19_x28" \
		$(BUILD)/readobj.txt
	rg -q "simple_virtio_block_outlined_capture_buffer_span_and_device" \
		$(BUILD)/readobj.txt
	rg -q "simple_virtio_storage_initialize_remote" $(BUILD)/readobj.txt
	rg -q "simple_virtio_storage_outlined_builder_span_x28" \
		$(BUILD)/readobj.txt
	rg -q "simple_virtio_storage_outlined_prepare_u32_field" \
		$(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_builder_initialize_sp20_x24" $(BUILD)/readobj.txt
	rg -q "usb_rpc_prepare_service_call" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_builder_initialize_sp20_x20" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_prepare_sp20_builder_field" $(BUILD)/readobj.txt
	rg -q "usb_rpc_require_response_field_zero" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_sp20_builder_span_x24" $(BUILD)/readobj.txt
	rg -q "usb_rpc_append_empty_field_zero" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_sp40_builder_span_x20" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_append_empty_field_zero_x20" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_save_field_span_x20_x23" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_prepare_empty_u32_field" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_prepare_sp28_byte_field_arguments" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_sp28_builder_span_x20" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_prepare_sp28_service_call" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_prepare_single_byte_field" $(BUILD)/readobj.txt
	rg -q "usb_rpc_require_sp28_response_field_zero" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_sp40_builder_span_x25" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_prepare_u32_field" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_append_u64_field_sp40" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_initialize_sp28_builder_arguments" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_prepare_single_byte_field_lengths" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_prepare_u16_field" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_load_builder_type_x19" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_sp28_builder_current_lower" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_clear_sp28_builder" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_select_field_type_x7" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_stage_source_span_and_load_builder_type_x23" $(BUILD)/readobj.txt
	rg -q "usb_rpc_device_deinitialize" $(BUILD)/readobj.txt
	rg -q "usb_rpc_device_initialize" $(BUILD)/readobj.txt
	rg -q "usb_rpc_build_configuration_descriptors" $(BUILD)/readobj.txt
	rg -q "usb_rpc_finalize_device_callbacks" $(BUILD)/readobj.txt
	rg -q "usb_rpc_register_transport_mapping" $(BUILD)/readobj.txt
	rg -q "usb_rpc_release_transport_mapping" $(BUILD)/readobj.txt
	rg -q "usb_rpc_commit_dfu_descriptors" $(BUILD)/readobj.txt
	rg -q "usb_rpc_command_0x11_set_byte" $(BUILD)/readobj.txt
	rg -q "usb_rpc_device_dispatch_slot0" $(BUILD)/readobj.txt
	rg -q "usb_rpc_device_dispatch_slot2" $(BUILD)/readobj.txt
	rg -q "usb_rpc_device_dispatch_slot3" $(BUILD)/readobj.txt
	rg -q "usb_rpc_device_dispatch_slot1" $(BUILD)/readobj.txt
	rg -q "usb_rpc_dfu_initialize" $(BUILD)/readobj.txt
	rg -q "usb_rpc_command_0x1a_set_byte" $(BUILD)/readobj.txt
	rg -q "usb_rpc_dfu_deinitialize" $(BUILD)/readobj.txt
	rg -q "usb_rpc_dfu_download_is_complete" $(BUILD)/readobj.txt
	rg -q "usb_rpc_dfu_download_event_handle" $(BUILD)/readobj.txt
	rg -q "usb_rpc_command_0x1f_set_byte" $(BUILD)/readobj.txt
	rg -q "usb_rpc_dfu_download_payload_length" $(BUILD)/readobj.txt
	rg -q "usb_rpc_simplevirtio_initialize" $(BUILD)/readobj.txt
	rg -q "ccn_neg" $(BUILD)/readobj.txt
	rg -q "cczp_n" $(BUILD)/readobj.txt
	rg -q "cczp_prime" $(BUILD)/readobj.txt
	rg -q "cczp_mod_ws" $(BUILD)/readobj.txt
	rg -q "cczp_mul_ws" $(BUILD)/readobj.txt
	rg -q "cczp_sqr_ws" $(BUILD)/readobj.txt
	rg -q "cczp_to_ws" $(BUILD)/readobj.txt
	rg -q "cczp_from_ws" $(BUILD)/readobj.txt
	rg -q "ccn_addmul1_asm" $(BUILD)/readobj.txt
	rg -q "ccn_mul" $(BUILD)/readobj.txt
	rg -q "ccn_mul1_asm" $(BUILD)/readobj.txt
	rg -q "ccn_addmul1" $(BUILD)/readobj.txt
	rg -q "ccn_mux" $(BUILD)/readobj.txt
	rg -q "ccn_cond_sub" $(BUILD)/readobj.txt
	rg -q "ccn_add_ws" $(BUILD)/readobj.txt
	rg -q "ccn_add_asm" $(BUILD)/readobj.txt
	rg -q "ccn_add1_ws" $(BUILD)/readobj.txt
	rg -q "ccn_mul_ws" $(BUILD)/readobj.txt
	rg -q "ccn_mul1" $(BUILD)/readobj.txt
	rg -q "ccn_shift_right" $(BUILD)/readobj.txt
	rg -q "ccn_sub_ws" $(BUILD)/readobj.txt
	rg -q "ccn_sub_asm" $(BUILD)/readobj.txt
	rg -q "ccn_sub1" $(BUILD)/readobj.txt
	rg -q "ccn_cond_add" $(BUILD)/readobj.txt
	rg -q "ccn_cond_neg" $(BUILD)/readobj.txt
	rg -q "ccn_shift_left" $(BUILD)/readobj.txt
	rg -q "ccn_shift_right_asm" $(BUILD)/readobj.txt
	rg -q "ccn_mux_next_mask" $(BUILD)/readobj.txt
	rg -q "ccn_shift_right_multi" $(BUILD)/readobj.txt
	rg -q "ccn_subn" $(BUILD)/readobj.txt
	rg -q "ccn_add1_asm" $(BUILD)/readobj.txt
	rg -q "ccsha384_vng_arm_hw_compress" $(BUILD)/readobj.txt
	rg -q "ccsha512_ltc_compress" $(BUILD)/readobj.txt
	rg -q "ccsha512_final" $(BUILD)/readobj.txt
	rg -q "AccelerateCrypto_SHA512_compress_hwassist" $(BUILD)/readobj.txt
	rg -q "AccelerateCrypto_SHA3_keccak" $(BUILD)/readobj.txt
	rg -q "usb_dfu_stack_policy_word_span" $(BUILD)/readobj.txt
	rg -q "usb_dfu_stack_set_policy_bit16" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_configuration_initialize_once" $(BUILD)/readobj.txt
	rg -q "usb_dfu_stack_release" $(BUILD)/readobj.txt
	rg -q "usb_device_transport_initialize" $(BUILD)/readobj.txt
	rg -q "usb_dfu_stack_acquire_tail_thunk" $(BUILD)/readobj.txt
	rg -q "usb_dfu_stack_acquire" $(BUILD)/readobj.txt
	rg -q "usb_dfu_device_lifecycle_state_noop" $(BUILD)/readobj.txt
	rg -q "usb_dfu_device_shutdown_local_noop" $(BUILD)/readobj.txt
	rg -q "usb_dfu_commit_descriptors_for_rpc_transport" $(BUILD)/readobj.txt
	rg -q "usb_device_registry_read_required_byte" $(BUILD)/readobj.txt
	rg -q "usb_dfu_stack_guard_load_to_x8" $(BUILD)/readobj.txt
	rg -q "usb_dfu_outlined_compare_policy_span_current_to_upper" $(BUILD)/readobj.txt
	rg -q "usb_dfu_outlined_prepare_policy_word_span" $(BUILD)/readobj.txt
	rg -q "usb_dfu_stack_guard_compare_saved_x8" $(BUILD)/readobj.txt
	rg -q "usb_device_local_deinitialize" $(BUILD)/readobj.txt
	rg -q "usb_device_local_initialize" $(BUILD)/readobj.txt
	rg -q "usb_device_local_command_0x11_set_byte" $(BUILD)/readobj.txt
	rg -q "usb_device_local_command_0x1a_set_byte" $(BUILD)/readobj.txt
	rg -q "usb_device_local_command_0x1f_set_byte" $(BUILD)/readobj.txt
	rg -q "usb_device_shared_buffers_initialize" $(BUILD)/readobj.txt
	rg -q "usb_device_shared_buffer_span" $(BUILD)/readobj.txt
	rg -q "usb_ep0_set_configuration_completion_noop" $(BUILD)/readobj.txt
	rg -q "usb_ep0_set_test_mode_complete" $(BUILD)/readobj.txt
	rg -q "usb_ep0_return_success_thunk" $(BUILD)/readobj.txt
	rg -q "usb_ep0_control_request_preflight_allow" $(BUILD)/readobj.txt
	rg -q "usb_device_validate_byte_span" $(BUILD)/readobj.txt
	rg -q "usb_outlined_return_success" $(BUILD)/readobj.txt
	rg -q "usb_device_make_byte_span" $(BUILD)/readobj.txt
	rg -q "usb_ep0_test_mode_selector" $(BUILD)/readobj.txt
	rg -q "usb_ep0_reset_control_state" $(BUILD)/readobj.txt
	rg -q "usb_ep0_reset_and_stall" $(BUILD)/readobj.txt
	rg -q "usb_ep0_submit_status_stage" $(BUILD)/readobj.txt
	rg -q "usb_transfer_outlined_publish_state_and_operations_span" $(BUILD)/readobj.txt
	rg -q "usb_transfer_validate_byte_span" $(BUILD)/readobj.txt
	rg -q "usb_queued_payload_node_tuple_validate" $(BUILD)/readobj.txt
	rg -q "usb_device_validate_controller_tuple" $(BUILD)/readobj.txt
	rg -q "usb_device_outlined_global_transfer_span" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_outlined_select_record_index_and_stride_w0_w8" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_create_setup_request" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_outlined_resolve_record_x8_x9" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_outlined_load_array_bounds_x19" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_outlined_select_record_index_and_stride_w8_w9" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_outlined_load_controller_view" $(BUILD)/readobj.txt
	rg -q "usb_outlined_stack_guard_compare_saved_x8" $(BUILD)/readobj.txt
	rg -q "usb_device_outlined_prepare_transport_event_arguments" $(BUILD)/readobj.txt
	rg -q "usb_transfer_outlined_move_and_clear_next_span" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_outlined_load_array_bounds_x8" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_set_in_phase_one_advance_to_out_x9" $(BUILD)/readobj.txt
	rg -q "usb_outlined_stack_guard_load_to_x8" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_outlined_compare_candidate_to_array_lower_x19" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_set_in_phase_advance_to_out_x8" $(BUILD)/readobj.txt
	rg -q "usb_transfer_request_complete_and_release" $(BUILD)/readobj.txt
	rg -q "usb_transfer_request_create_and_submit" $(BUILD)/readobj.txt
	rg -q "usb_ep0_validate_descriptor_span_thunk" $(BUILD)/readobj.txt
	rg -q "usb_transfer_request_validate_tuple" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_clear_transfer_span" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_load_payload_bounds" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_load_payload_span" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_setup_request_span_x25" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_load_endpoint_callback_span_x9" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_select_endpoint_callback_record" $(BUILD)/readobj.txt
	rg -q "usb_transfer_outlined_initialize_request_tail" $(BUILD)/readobj.txt
	rg -q "usb_transfer_outlined_restore_request_span_x22_x19" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_clamp_configuration_descriptor_length" $(BUILD)/readobj.txt
	rg -q "usb_transfer_request_allocate_one" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_transfer_worker_start_hook_noop" $(BUILD)/readobj.txt
	rg -q "usb_submit_status_two_response_masked" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_transfer_worker" $(BUILD)/readobj.txt
	rg -q "usb_ep0_setup_transfer_complete" $(BUILD)/readobj.txt
	rg -q "usb_transfer_submit" $(BUILD)/readobj.txt
	rg -q "usb_transfer_enqueue" $(BUILD)/readobj.txt
	rg -q "usb_ep0_control_transfer_complete" $(BUILD)/readobj.txt
	rg -q "usb_submit_ep0_setup_request" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_cancel_pending_transfers" $(BUILD)/readobj.txt
	rg -q "usb_transfer_status_unsupported" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_payload_queue_append" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_worker_event_create" $(BUILD)/readobj.txt
	rg -q "usb_ep0_in_transfer_request_create" $(BUILD)/readobj.txt
	rg -q "usb_transfer_request_create" $(BUILD)/readobj.txt
	rg -q "j_usb_device_dispatch_transfer_completion_optional" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_control_transfer_buffer_release" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_shutdown" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_table_install_or_clear" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot0_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot1_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot2_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot3_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_set_address" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot4_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot5" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot5_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_transfer_submit" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot7_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_endpoint_set_halt" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot8_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot9" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot9_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot10" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot10_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_set_test_mode" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot11_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot12" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot12_missing_callback_fatal" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_slot14_optional" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_transfer_completion_optional" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_source_token" $(BUILD)/readobj.txt
	rg -q "usb_device_dispatch_table_span" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_build_return_success_thunk" $(BUILD)/readobj.txt
	rg -q "usb_build_dfu_descriptors" $(BUILD)/readobj.txt
	rg -q "usb_build_string_descriptor" $(BUILD)/readobj.txt
	rg -q "usb_insert_string_descriptor" $(BUILD)/readobj.txt
	rg -q "usb_build_configuration_descriptors" $(BUILD)/readobj.txt
	rg -q "usb_function_register_implementation_span" $(BUILD)/readobj.txt
	rg -q "usb_function_registration_bounds_panic" $(BUILD)/readobj.txt
	rg -q "usb_outlined_load_device_state_x19" $(BUILD)/readobj.txt
	rg -q "usb_function_outlined_load_implementation_bounds" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_outlined_stack_guard_compare_saved_x8" $(BUILD)/readobj.txt
	rg -q "usb_dfu_device_initialize" $(BUILD)/readobj.txt
	rg -q "usb_dfu_outlined_prepare_descriptor_arguments" $(BUILD)/readobj.txt
	rg -q "usb_device_install_callback_pair" $(BUILD)/readobj.txt
	rg -q "usb_device_prepare_resources" $(BUILD)/readobj.txt
	rg -q "usb_control_transfer_buffer_capacity" $(BUILD)/readobj.txt
	rg -q "usb_control_transfer_buffer_release" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_initialize" $(BUILD)/readobj.txt
	rg -q "usb_device_transport_event_callback" $(BUILD)/readobj.txt
	rg -q "usb_device_shutdown" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_deinitialize" $(BUILD)/readobj.txt
	rg -q "usb_device_signal_worker" $(BUILD)/readobj.txt
	rg -q "usb_device_method_slot4_noop" $(BUILD)/readobj.txt
	rg -q "usb_device_method_slot5_return_two" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_set_halt" $(BUILD)/readobj.txt
	rg -q "usb_device_method_slot9_noop" $(BUILD)/readobj.txt
	rg -q "usb_endpoint_is_halted" $(BUILD)/readobj.txt
	rg -q "usb_device_method_slot11_noop" $(BUILD)/readobj.txt
	rg -q "usb_device_lifecycle_transition" $(BUILD)/readobj.txt
	rg -q "usb_device_finalize_callbacks" $(BUILD)/readobj.txt
	rg -q "usb_device_deinitialize" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_stack_guard_compare_fp_minus_18" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_stack_guard_compare_fp_minus_38" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_frame90_epilogue" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_frame90_prologue" $(BUILD)/readobj.txt
	rg -q "usb_rpc_stack_guard_load_to_x8" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_stack_guard_save_fp_minus_18" $(BUILD)/readobj.txt
	rg -q "usb_rpc_outlined_stack_guard_compare_saved_x8" $(BUILD)/readobj.txt
	rg -q "j_usb_dfu_stack_acquire" $(BUILD)/readobj.txt
	rg -q "usb_dfu_device_start" $(BUILD)/readobj.txt
	rg -q "usb_dfu_device_shutdown" $(BUILD)/readobj.txt
	rg -q "usb_dfu_device_deinitialize" $(BUILD)/readobj.txt
	rg -q "usb_dispatch_control_request" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_load_qword_type_x3" $(BUILD)/readobj.txt
	rg -q "usb_ep0_outlined_copy_saved_source_span_x24_x21" $(BUILD)/readobj.txt
	rg -q "root_rpc_usb_lifecycle_notify" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_endpoint_slot_fatal" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_local_initialize" $(BUILD)/readobj.txt
	rg -q "sep_runtime_transport_stack_guard_compare_x8" $(BUILD)/readobj.txt
	rg -q "sep_runtime_stack_guard_compare" $(BUILD)/readobj.txt
	rg -q "sep_runtime_stack_guard_load" $(BUILD)/readobj.txt
	rg -q "j_sep_bootstrap_configuration_exchange" $(BUILD)/readobj.txt
	rg -q "sep_descriptor_address_add_checked" $(BUILD)/readobj.txt
	rg -q "simplevirtio_device_bounded_view" $(BUILD)/readobj.txt
	rg -q "simple_virtio_storage_stack_guard_compare_saved_x8" $(BUILD)/readobj.txt
	rg -q "platform_progress_publish_code" $(BUILD)/readobj.txt
	rg -q "platform_progress_publish_words" $(BUILD)/readobj.txt
	rg -q "platform_progress_clear" $(BUILD)/readobj.txt
	rg -q "platform_progress_read_code" $(BUILD)/readobj.txt
	rg -q "boot_progress_publish_source_attempt_state" $(BUILD)/readobj.txt
	rg -q "boot_progress_publish_dfu_active_state" $(BUILD)/readobj.txt
	rg -q "avp_reset_entry" $(BUILD)/readobj.txt
	rg -q "avp_reset_early_platform_hook_noop" $(BUILD)/readobj.txt
	rg -q "platform_system_reset" $(BUILD)/readobj.txt
	rg -q "platform_psci_system_reset" $(BUILD)/readobj.txt
	rg -q "platform_psci_require_supported_version" $(BUILD)/readobj.txt
	rg -q "platform_psci_unsupported_version_fatal" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_select_latest_source_offset" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_scan_redundant_headers" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_log_generated_location" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_prepare_selection_progress" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_prepare_provider_read" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_compare_header_magic" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_no_header_progress_code" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_dispatch_provider_read" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_clear_header_candidates" $(BUILD)/readobj.txt
	rg -q "runtime_stack_guard_compare_saved_x8_provider_scan" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_capture_provider_tuple_and_stack_guard" $(BUILD)/readobj.txt
	rg -q "runtime_stack_guard_load_to_x8_provider_scan" $(BUILD)/readobj.txt
	rg -q "boot_virtual_firmware_restore_provider_tuple" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_emit_code_hook_noop" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_emit_code_with_detail_hook_noop" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_primary_sink_write_unsupported" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_fallback_sink_write_unsupported" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_emit_code" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_buffer_rollover_if_needed" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_buffer_write_span" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_buffer_write_index_out_of_range_fatal" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_buffer_remaining_capacity" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_buffer_exhausted_fatal" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_emit_code_with_detail" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_append_label_if_enabled" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_append_label" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_finalize_and_commit" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_commit" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_buffer_compact_after_rollover" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_buffer_prepend_tail_tokens" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_validate_byte_tuple" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_source_token" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_outlined_compare_base_plus_offset_to_upper" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_outlined_restore_span_bounds_x21_x20_x19" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_outlined_save_span_bounds_x21_x20_x19" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_outlined_load_byte_type_x3" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_outlined_restore_span_x23_x26" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_outlined_save_span_x19_x22" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_buffer_advance_write_offset" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_recording_enabled_compare" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_record_category_flag" $(BUILD)/readobj.txt
	rg -q "boot_breadcrumb_mark_commit_pending" $(BUILD)/readobj.txt
	rg -q "j_runtime_data_memory_barrier_system" $(BUILD)/readobj.txt
	rg -q "runtime_data_memory_barrier_system" $(BUILD)/readobj.txt
	rg -q "runtime_write_apiakey_el1" $(BUILD)/readobj.txt
	rg -q "runtime_write_apibkey_el1" $(BUILD)/readobj.txt
	rg -q "runtime_write_apdakey_el1" $(BUILD)/readobj.txt
	rg -q "runtime_write_apdbkey_el1" $(BUILD)/readobj.txt
	rg -q "runtime_write_apgakey_el1" $(BUILD)/readobj.txt
	rg -q "runtime_cpu_feature_registers_export" $(BUILD)/readobj.txt
	rg -q "runtime_write_tpidr_el1" $(BUILD)/readobj.txt
	rg -q "runtime_read_isr_el1" $(BUILD)/readobj.txt
	rg -q "runtime_read_mpidr_affinity_level1" $(BUILD)/readobj.txt
	rg -q "runtime_read_mpidr_el1" $(BUILD)/readobj.txt
	rg -q "runtime_enable_sctlr_el1_pointer_authentication" $(BUILD)/readobj.txt
	rg -q "runtime_selected_task_save_control_registers" $(BUILD)/readobj.txt
	rg -q "runtime_format_update_length_modifier_flags" $(BUILD)/readobj.txt
	rg -q "runtime_format_stack_guard_load_to_x9" $(BUILD)/readobj.txt
	rg -q "runtime_format_write_two_byte_scratch" $(BUILD)/readobj.txt
	rg -q "runtime_format_prepare_literal_output" $(BUILD)/readobj.txt
	rg -q "runtime_format_consume_argument_to_x8" $(BUILD)/readobj.txt
	rg -q "runtime_format_advance_argument_cursor" $(BUILD)/readobj.txt
	rg -q "runtime_format_write_fourteen_byte_scratch" $(BUILD)/readobj.txt
	rg -q "runtime_format_save_primary_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_virtual_timer_control_read" $(BUILD)/readobj.txt
	rg -q "runtime_virtual_timer_control_write" $(BUILD)/readobj.txt
	rg -q "runtime_virtual_timer_tval_write" $(BUILD)/readobj.txt
	rg -q "runtime_wait_for_interrupt_once" $(BUILD)/readobj.txt
	rg -q "runtime_interrupt_mask_push_svc" $(BUILD)/readobj.txt
	rg -q "runtime_interrupt_mask_pop_svc" $(BUILD)/readobj.txt
	rg -q "runtime_virtual_timer_irq_post_initialize_noop" $(BUILD)/readobj.txt
	rg -q "runtime_interrupt_worker_case1_hook_noop" $(BUILD)/readobj.txt
	rg -q "runtime_current_task_interrupt_mask_depth_increment" $(BUILD)/readobj.txt
	rg -q "runtime_current_task_interrupt_mask_depth_decrement" $(BUILD)/readobj.txt
	rg -q "runtime_current_task_interrupt_mask_depth_is_zero" $(BUILD)/readobj.txt
	rg -q "runtime_interrupt_source_token" $(BUILD)/readobj.txt
	rg -q "runtime_interrupt_state_flags_compare_sp_current_to_upper" $(BUILD)/readobj.txt
	rg -q "runtime_interrupt_state_flags_prepare_sp_result_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_interrupt_state_flags_bounded_view" $(BUILD)/readobj.txt
	rg -q "runtime_interrupt_worker" $(BUILD)/readobj.txt
	rg -q "runtime_interrupt_worker_state_flags_to_stack" $(BUILD)/readobj.txt
	rg -q "j_runtime_wait_for_interrupt_once" $(BUILD)/readobj.txt
	rg -q "runtime_event_create_svc" $(BUILD)/readobj.txt
	rg -q "runtime_event_wait_forever" $(BUILD)/readobj.txt
	rg -q "runtime_event_wait_with_timeout" $(BUILD)/readobj.txt
	rg -q "runtime_event_set_timer_callback" $(BUILD)/readobj.txt
	rg -q "runtime_event_static_initialization_duplicate_fatal" $(BUILD)/readobj.txt
	rg -q "runtime_event_blocking_wait_is_enabled" $(BUILD)/readobj.txt
	rg -q "runtime_event_resolve_object_id" $(BUILD)/readobj.txt
	rg -q "runtime_event_assign_signal_state" $(BUILD)/readobj.txt
	rg -q "runtime_event_initialize_static_records" $(BUILD)/readobj.txt
	rg -q "runtime_event_registration_tuple_validate" $(BUILD)/readobj.txt
	rg -q "runtime_current_domain_index_equals" $(BUILD)/readobj.txt
	rg -q "runtime_current_domain_index_bounded_view" $(BUILD)/readobj.txt
	rg -q "runtime_current_domain_index" $(BUILD)/readobj.txt
	rg -q "runtime_domain_resolve_object_id" $(BUILD)/readobj.txt
	rg -q "runtime_domain_registry_contains_id" $(BUILD)/readobj.txt
	rg -q "runtime_current_domain_identity_bounded_view" $(BUILD)/readobj.txt
	rg -q "runtime_current_domain_handle_bounded_view" $(BUILD)/readobj.txt
	rg -q "runtime_domain_identity_tuple_validate" $(BUILD)/readobj.txt
	rg -q "runtime_current_domain_metadata_publish" $(BUILD)/readobj.txt
	rg -q "runtime_domain_record_bounds_panic" $(BUILD)/readobj.txt
	rg -q "runtime_domain_selector_lookup_id" $(BUILD)/readobj.txt
	rg -q "runtime_domain_selector_decode_nibble_position" $(BUILD)/readobj.txt
	rg -q "runtime_domain_public_handle_for_id" $(BUILD)/readobj.txt
	rg -q "runtime_current_domain_handle" $(BUILD)/readobj.txt
	rg -q "runtime_domain_resolve_wait_signal_capability_from_x10_record" $(BUILD)/readobj.txt
	rg -q "runtime_current_task_unset_fatal" $(BUILD)/readobj.txt
	rg -q "runtime_task_capability_tuple_validate_thunk_a" $(BUILD)/readobj.txt
	rg -q "runtime_task_capability_tuple_validate_thunk_b" $(BUILD)/readobj.txt
	rg -q "runtime_task_source_token" $(BUILD)/readobj.txt
	rg -q "runtime_task_capability_tuple_validate" $(BUILD)/readobj.txt
	rg -q "runtime_task_start_trampoline" $(BUILD)/readobj.txt
	rg -q "runtime_task_start_validate_domain_allocator_route_thunk_a" $(BUILD)/readobj.txt
	rg -q "runtime_task_start_validate_domain_allocator_route_thunk_b" $(BUILD)/readobj.txt
	rg -q "runtime_task_set_state" $(BUILD)/readobj.txt
	rg -q "runtime_task_make_ready" $(BUILD)/readobj.txt
	rg -q "runtime_task_make_ready_bounds_panic" $(BUILD)/readobj.txt
	rg -q "boot_dispatch_validate_dfu_download_span" $(BUILD)/readobj.txt
	rg -q "j_platform_set_dfu_mode_noop" $(BUILD)/readobj.txt
	rg -q "boot_finalize_usb_and_platform_services" $(BUILD)/readobj.txt
	rg -q "boot_finalize_sep_and_platform_state" $(BUILD)/readobj.txt
	rg -q "platform_claim_force_dfu" $(BUILD)/readobj.txt
	rg -q "platform_set_dfu_mode_noop" $(BUILD)/readobj.txt
	rg -q "platform_boot_dfu_image_type_ibss" $(BUILD)/readobj.txt
	rg -q "boot_platform_prepare_sep_shutdown_noop" $(BUILD)/readobj.txt
	rg -q "boot_dispatch_preflight_hook_noop" $(BUILD)/readobj.txt
	rg -q "boot_context_populate_platform" $(BUILD)/readobj.txt
	rg -q "boot_context_initialize" $(BUILD)/readobj.txt
	rg -q "boot_image_expected_manifest_digest_callback" $(BUILD)/readobj.txt
	rg -q "boot_image_expected_manifest_digest_bounds_fatal" $(BUILD)/readobj.txt
	rg -q "boot_img4_validate_byte_span_thunk_a" $(BUILD)/readobj.txt
	rg -q "boot_image_validate_source_registry_node_span" $(BUILD)/readobj.txt
	rg -q "boot_img4_validate_byte_span_thunk_b" $(BUILD)/readobj.txt
	rg -q "boot_image_validate_property_context_span" $(BUILD)/readobj.txt
	rg -q "boot_image_stack_guard_load_to_x9" $(BUILD)/readobj.txt
	rg -q "boot_image_stack_guard_load_to_x8" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_restore_provider_descriptor_bounds" $(BUILD)/readobj.txt
	rg -q "boot_img4_outlined_prepare_consumed_length_output" $(BUILD)/readobj.txt
	rg -q "boot_image_breadcrumb_code_base" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_restore_output_span_x19" $(BUILD)/readobj.txt
	rg -q "boot_image_property_breadcrumb_code_base" $(BUILD)/readobj.txt
	rg -q "boot_img4_validate_byte_span" $(BUILD)/readobj.txt
	rg -q "boot_img4_outlined_clear_payload_type_output" $(BUILD)/readobj.txt
	rg -q "boot_image_publish_sha384_digest_info" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_load_byte_type_to_x7" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_restore_header_storage_span" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_save_provider_descriptor_bounds" $(BUILD)/readobj.txt
	rg -q "boot_img4_compare_property_data" $(BUILD)/readobj.txt
	rg -q "boot_image_property_outlined_set_true_and_clear_status" $(BUILD)/readobj.txt
	rg -q "boot_image_property_breadcrumb_code_base_to_w19" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_restore_image_span_x23_x20" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_restore_span_upper_and_type_x28_x27" $(BUILD)/readobj.txt
	rg -q "boot_img4_validate_byte_span_clone" $(BUILD)/readobj.txt
	rg -q "boot_img4_validate_integer_property" $(BUILD)/readobj.txt
	rg -q "boot_img4_validate_boolean_property" $(BUILD)/readobj.txt
	rg -q "boot_img4_validate_data_property" $(BUILD)/readobj.txt
	rg -q "boot_img4_validate_true_boolean_property" $(BUILD)/readobj.txt
	rg -q "boot_img4_decode_fourcc_be" $(BUILD)/readobj.txt
	rg -q "boot_image_mark_property_0x6575726d_true" $(BUILD)/readobj.txt
	rg -q "boot_img4_compare_der_integer_tag" $(BUILD)/readobj.txt
	rg -q "boot_img4_outlined_prepare_partial_decode_arguments" $(BUILD)/readobj.txt
	rg -q "boot_img4_outlined_prepare_der_item_span" $(BUILD)/readobj.txt
	rg -q "boot_img4_stack_guard_compare_saved_x8" $(BUILD)/readobj.txt
	rg -q "boot_image_register_provider_frame100_restore_x19_x28" $(BUILD)/readobj.txt
	rg -q "boot_image_register_provider_frame100_save_x19_x28" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_load_global_page_bases_x20_x22" $(BUILD)/readobj.txt
	rg -q "boot_image_source_lookup_prepare_registry_span_slots" $(BUILD)/readobj.txt
	rg -q "boot_image_source_lookup_prepare_next_span_slots" $(BUILD)/readobj.txt
	rg -q "boot_image_source_lookup_validate_current_node" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_restore_storage_span_to_x4_x7" $(BUILD)/readobj.txt
	rg -q "boot_image_validate_property_callback_context" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_validate_output_span_and_add_x22" $(BUILD)/readobj.txt
	rg -q "boot_image_validate_source_allocation_span" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_allocation_span_x19_x22" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_restore_source_record_bounds_x26_x24" $(BUILD)/readobj.txt
	rg -q "boot_image_stack_guard_compare_saved_x8_clone" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_save_source_record_bounds_x26_x24" $(BUILD)/readobj.txt
	rg -q "boot_image_stack_guard_load_to_x8_clone" $(BUILD)/readobj.txt
	rg -q "boot_image_load_error_code_base_to_w20" $(BUILD)/readobj.txt
	rg -q "boot_image_outlined_save_allocation_span_x19_x22" $(BUILD)/readobj.txt
	rg -q "platform_watchdog_service_noop" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_reset" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_memory_bounds_update" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_memory_bounds_overflow_fatal" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_flags_accept_and_promote_request" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_select_flag5_profile" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_memory_span_allowed" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_flag24_set" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_flag22_is_set" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_compare_production_mode_byte" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_set_entropy_seed_span" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_set_entropy_context_word" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_flag25_set" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_flag27_set" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_flag30_set" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_flag42_set" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_flag40_is_clear" $(BUILD)/readobj.txt
	rg -q "platform_boot_source_attempt_completion_progress_is_enabled" $(BUILD)/readobj.txt
	rg -q "platform_boot_normal_image_type_illb" $(BUILD)/readobj.txt
	rg -q "platform_boot_policy_flag27_default_is_set" $(BUILD)/readobj.txt
	rg -q "platform_memory_policy_range_is_exempt" $(BUILD)/readobj.txt
	rg -q "j_platform_boot_source_attempt_completion_progress_is_enabled" $(BUILD)/readobj.txt
	rg -q "j_platform_boot_policy_memory_span_allowed" $(BUILD)/readobj.txt
	rg -q "platform_boot_handoff_trampoline_bounded_view" $(BUILD)/readobj.txt
	rg -q "j_platform_boot_handoff_trampoline_bounded_view" $(BUILD)/readobj.txt
	rg -q "boot_root_firmware_services_start_hook_noop" $(BUILD)/readobj.txt
	rg -q "boot_platform_shutdown_phase_hook_noop" $(BUILD)/readobj.txt
	rg -q "boot_image_return_true_thunk" $(BUILD)/readobj.txt
	rg -q "platform_mmu_prepare_noop_return_true" $(BUILD)/readobj.txt
	rg -q "boot_platform_prepare_payload_handoff_noop" $(BUILD)/readobj.txt
	rg -q "boot_platform_payload_handoff_policy_hook_noop" $(BUILD)/readobj.txt
	rg -q "boot_root_firmware_services_prepare_status_zero" $(BUILD)/readobj.txt
	rg -q "boot_platform_payload_handoff_context" $(BUILD)/readobj.txt
	rg -q "boot_mmu_map_16k_range_x20_x21_x22_w23" $(BUILD)/readobj.txt
	rg -q "boot_mmu_configure_mair_el1" $(BUILD)/readobj.txt
	rg -q "boot_mmu_build_sctlr_el1_value" $(BUILD)/readobj.txt
	rg -q "boot_mmu_build_and_activate_relocated_address_space" $(BUILD)/readobj.txt
	rg -q "boot_mmu_remove_temporary_work_ram_mappings_x12" $(BUILD)/readobj.txt
	rg -q "platform_read_production_mode_config_0x08" $(BUILD)/readobj.txt
	rg -q "platform_read_production_mode_config_0x08_clone" $(BUILD)/readobj.txt
	rg -q "platform_read_production_mode_config_0x0c" $(BUILD)/readobj.txt
	rg -q "platform_read_security_domain" $(BUILD)/readobj.txt
	rg -q "platform_read_config_0x10" $(BUILD)/readobj.txt
	rg -q "platform_read_security_epoch" $(BUILD)/readobj.txt
	rg -q "platform_get_chip_id" $(BUILD)/readobj.txt
	rg -q "platform_read_chip_revision" $(BUILD)/readobj.txt
	rg -q "platform_read_ecid" $(BUILD)/readobj.txt
	rg -q "platform_read_config_u32_shared" $(BUILD)/readobj.txt
	rg -q "platform_read_chip_production_fuse_config" $(BUILD)/readobj.txt
	rg -q "j_platform_read_security_epoch" $(BUILD)/readobj.txt
	rg -q "j_platform_read_config_0x10" $(BUILD)/readobj.txt
	rg -q "j_platform_read_production_mode_config_0x08" $(BUILD)/readobj.txt
	rg -q "j_platform_read_production_mode_config_0x08_clone" $(BUILD)/readobj.txt
	rg -q "j_platform_read_security_domain" $(BUILD)/readobj.txt
	rg -q "j_platform_read_security_epoch_clone" $(BUILD)/readobj.txt
	rg -q "platform_identity_static_flag_default_false" $(BUILD)/readobj.txt
	rg -q "j_platform_read_chip_production_fuse_config" $(BUILD)/readobj.txt
	rg -q "j_platform_get_chip_id" $(BUILD)/readobj.txt
	rg -q "j_platform_read_chip_revision" $(BUILD)/readobj.txt
	rg -q "j_platform_read_ecid" $(BUILD)/readobj.txt
	rg -q "boot_img4_parse_container_header" $(BUILD)/readobj.txt
	rg -q "boot_img4_decode_expected_der_tag_partial" $(BUILD)/readobj.txt
	rg -q "boot_img4_limit_der_item_to_decoded_content" $(BUILD)/readobj.txt
	rg -q "boot_img4_consume_expected_der_string" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_is_16k_aligned" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_alignment_bounds_panic" $(BUILD)/readobj.txt
	rg -q "j_runtime_physical_memory_region_is_live" $(BUILD)/readobj.txt
	rg -q "j_runtime_physical_memory_region_is_live_clone" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_policy_class" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_active_base_bounds_panic" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_deactivate_alias" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_activate_alias" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_activate_alias_with_flags" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_initialize_fixed_registry_view" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_count_span" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_policy_class_limit_span" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_policy_class_table_span" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_apply_region_records" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_regions_initialized_flag_bounded_view" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_uses_supervisor_services" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_bounds_from_x23" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_region_is_defined_bounds_panic" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_outlined_restore_region_tuple_x22_x19" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_outlined_save_region_tuple_x19_x21_x20" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_outlined_save_region_tuple_and_test_live" $(BUILD)/readobj.txt
	rg -q "parse_chain" $(BUILD)/readobj.txt
	rg -q "_crack_chain_with_anchor" $(BUILD)/readobj.txt
	rg -q "parse_extensions" $(BUILD)/readobj.txt
	rg -q "parse_algorithm_identifier" $(BUILD)/readobj.txt
	rg -q "ccrsa_verify_pkcs1v15_internal_ws" $(BUILD)/readobj.txt
	rg -q "ccrsa_make_pub" $(BUILD)/readobj.txt
	rg -q "ccrsa_emsa_pkcs1v15_verify_canary_out" $(BUILD)/readobj.txt
	rg -q "ccrsa_pub_crypt_ws" $(BUILD)/readobj.txt
	rg -q "ccrsa_verify_pkcs1v15" $(BUILD)/readobj.txt
	rg -q "ccrsa_verify_pkcs1v15_digest" $(BUILD)/readobj.txt
	rg -q "cczp_mm_init_ws" $(BUILD)/readobj.txt
	rg -q "cczp_mm_to_ws" $(BUILD)/readobj.txt
	rg -q "cczp_mm_from_ws" $(BUILD)/readobj.txt
	rg -q "cczp_mm_mul_ws" $(BUILD)/readobj.txt
	rg -q "cczp_mm_sqr_ws" $(BUILD)/readobj.txt
	rg -q "cczp_mm_mod_ws" $(BUILD)/readobj.txt
	rg -q "cczp_from_default_ws" $(BUILD)/readobj.txt
	rg -q "cczp_to_default_ws" $(BUILD)/readobj.txt
	rg -q "cczp_is_quadratic_residue_ws" $(BUILD)/readobj.txt
	rg -q "cczp_inv_default_ws" $(BUILD)/readobj.txt
	rg -q "cczp_inv_update_redc_ws" $(BUILD)/readobj.txt
	rg -q "cczp_sqrt_tonelli_shanks_precomp_ws" $(BUILD)/readobj.txt
	rg -q "cczp_sqrt_default_ws" $(BUILD)/readobj.txt
	rg -q "cczp_power_fast_ws" $(BUILD)/readobj.txt
	rg -q "ccn_gcd_approximate" $(BUILD)/readobj.txt
	rg -q "ccn_gcd_update_ws" $(BUILD)/readobj.txt
	rg -q "cczp_cond_negate" $(BUILD)/readobj.txt
	rg -q "cczp_mm_redc_ws" $(BUILD)/readobj.txt
	rg -q "cczp_add_default_ws" $(BUILD)/readobj.txt
	rg -q "cczp_init_ws" $(BUILD)/readobj.txt
	rg -q "cczp_modn_ws" $(BUILD)/readobj.txt
	rg -q "cczp_mod_default_ws" $(BUILD)/readobj.txt
	rg -q "cczp_mul_default_ws" $(BUILD)/readobj.txt
	rg -q "cczp_sqr_default_ws" $(BUILD)/readobj.txt
	rg -q "cczp_sub_default_ws" $(BUILD)/readobj.txt
	rg -q "ccn_sqr_ws" $(BUILD)/readobj.txt
	rg -q "ccn_divmod_ws" $(BUILD)/readobj.txt
	rg -q "verify_pkcs1_sig" $(BUILD)/readobj.txt
	rg -q "verify_signature_rsa" $(BUILD)/readobj.txt
	rg -q "img4_make_rsa_public_key_checked" $(BUILD)/readobj.txt
	rg -q "verify_chain_signatures" $(BUILD)/readobj.txt
	rg -q "DERImg4Decode" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodePayloadWithProperties" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodePayloadProperties" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodePayload" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodeManifest" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodeManifestCommon" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodeRestoreInfo" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodeProperty" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodeFindPropertyInSequence" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodeContentFindItemWithTag" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodePayloadCompression" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodePropertyWithItem" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodeParseManifestProperties" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetPayload" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetPayloadType" $(BUILD)/readobj.txt
	rg -q "Img4DecodePayloadCompressionInfoExists" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetPayloadCompressionInfo" $(BUILD)/readobj.txt
	rg -q "Img4DecodeManifestExists" $(BUILD)/readobj.txt
	rg -q "Img4DecodeSectionExists" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetRestoreInfoData" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetDataFromSection" $(BUILD)/readobj.txt
	rg -q "Img4DecodePayloadPropertiesExists" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetPayloadProperties" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetPropertyFromSection" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetPropertyData" $(BUILD)/readobj.txt
	rg -q "Img4DecodeInit" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodeFindProperty" $(BUILD)/readobj.txt
	rg -q "DERImg4DecodeParseManifestPropertiesInternal" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetPropertyInteger64" $(BUILD)/readobj.txt
	rg -q "Img4DecodeGetPropertyBoolean" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_initialize_u32_zero_once" $(BUILD)/readobj.txt
	rg -q "runtime_bootstrap_memory_optional_hook_noop" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_return_null_span_shared_tail" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_frame90_restore_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_frame90_save_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_task_interrupt_mask_test_bounded" $(BUILD)/readobj.txt
	rg -q "runtime_address_space_outlined_save_mapping_tuple_x21_x22_x20" $(BUILD)/readobj.txt
	rg -q "runtime_physical_region_outlined_restore_bounds_x20_x23_x24" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_save_x0_x2_to_x20_x22" $(BUILD)/readobj.txt
	rg -q "runtime_task_stack_outlined_bitmap_span_x21" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_save_x0_x1_to_x20_x19" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_save_x0_x2_to_x23_x25" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_load_global_state_to_x22" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_load_platform_configuration_to_x22" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_frame_c0_save_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_frame70_restore_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_frame_a0_restore_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_frame70_save_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_outlined_save_x0_x2_to_x19_x21" $(BUILD)/readobj.txt
	rg -q "runtime_return_null" $(BUILD)/readobj.txt
	rg -q "runtime_return_minus_one" $(BUILD)/readobj.txt
	rg -q "runtime_exception_svc_entry" $(BUILD)/readobj.txt
	rg -q "runtime_exception_lower_sync_full_save_entry" $(BUILD)/readobj.txt
	rg -q "runtime_exception_full_frame_dispatch" $(BUILD)/readobj.txt
	rg -q "runtime_exception_handler_stack_pointer" $(BUILD)/readobj.txt
	rg -q "runtime_exception_validate_handler_stack" $(BUILD)/readobj.txt
	rg -q "runtime_exception_outlined_save_ttbr0_ttbr1_x21" $(BUILD)/readobj.txt
	rg -q "runtime_task_interrupt_mask_set" $(BUILD)/readobj.txt
	rg -q "runtime_task_interrupt_mask_set_bounds_panic" $(BUILD)/readobj.txt
	rg -q "runtime_task_interrupt_mask_clear" $(BUILD)/readobj.txt
	rg -q "runtime_task_interrupt_mask_clear_bounds_panic" $(BUILD)/readobj.txt
	rg -q "runtime_task_interrupt_mask_is_set" $(BUILD)/readobj.txt
	rg -q "runtime_task_interrupt_mask_test_bounds_panic" $(BUILD)/readobj.txt
	rg -q "runtime_current_task_or_null" $(BUILD)/readobj.txt
	rg -q "runtime_current_task_bounded_view" $(BUILD)/readobj.txt
	rg -q "runtime_current_task_is_bootstrap_sentinel" $(BUILD)/readobj.txt
	rg -q "runtime_selected_task_bounded_view" $(BUILD)/readobj.txt
	rg -q "runtime_task_exception_frame_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_task_require_blocking_allowed" $(BUILD)/readobj.txt
	rg -q "runtime_task_shared_page_span_matches" $(BUILD)/readobj.txt
	rg -q "runtime_task_tuple_validate" $(BUILD)/readobj.txt
	rg -q "runtime_task_lookup_by_domain_id" $(BUILD)/readobj.txt
	rg -q "runtime_console_input_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_console_input_ring_push_notify" $(BUILD)/readobj.txt
	rg -q "runtime_console_input_ring_pop_notify" $(BUILD)/readobj.txt
	rg -q "runtime_console_output_buffered_write" $(BUILD)/readobj.txt
	rg -q "runtime_console_output_ring_write_all" $(BUILD)/readobj.txt
	rg -q "runtime_console_output_drain_signal" $(BUILD)/readobj.txt
	rg -q "runtime_console_discard_span" $(BUILD)/readobj.txt
	rg -q "runtime_console_write_cstring" $(BUILD)/readobj.txt
	rg -q "runtime_console_write_span" $(BUILD)/readobj.txt
	rg -q "runtime_console_outlined_restore_span_x22_x19" $(BUILD)/readobj.txt
	rg -q "runtime_console_outlined_save_span_x22_x19" $(BUILD)/readobj.txt
	rg -q "runtime_console_outlined_restore_span_bounds_x21_x19" $(BUILD)/readobj.txt
	rg -q "runtime_console_outlined_lock_tuple_x26" $(BUILD)/readobj.txt
	rg -q "runtime_console_outlined_lock_tuple_x25" $(BUILD)/readobj.txt
	rg -q "runtime_console_write_cstring_status" $(BUILD)/readobj.txt
	rg -q "runtime_page_table_level_shift" $(BUILD)/readobj.txt
	rg -q "runtime_page_table_root_level" $(BUILD)/readobj.txt
	rg -q "runtime_timer_program_head" $(BUILD)/readobj.txt
	rg -q "runtime_task_timer_expired" $(BUILD)/readobj.txt
	rg -q "runtime_task_event_timeout" $(BUILD)/readobj.txt
	rg -q "runtime_event_wake_one" $(BUILD)/readobj.txt
	rg -q "runtime_bounded_hex_encode_uppercase" $(BUILD)/readobj.txt
	rg -q "runtime_bounded_hex_encode_source_token" $(BUILD)/readobj.txt
	rg -q "runtime_vprintf" $(BUILD)/readobj.txt
	rg -q "runtime_vprintf_with_options" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_vsnprintf" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_vsnprintf_with_options" $(BUILD)/readobj.txt
	rg -q "runtime_printf" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_snprintf" $(BUILD)/readobj.txt
	rg -q "runtime_vsnprintf" $(BUILD)/readobj.txt
	rg -q "runtime_format_vwrite" $(BUILD)/readobj.txt
	rg -q "runtime_format_console_write" $(BUILD)/readobj.txt
	rg -q "runtime_format_output_write" $(BUILD)/readobj.txt
	rg -q "runtime_format_render_conversion" $(BUILD)/readobj.txt
	rg -q "runtime_format_update_field_validity" $(BUILD)/readobj.txt
	rg -q "runtime_format_null_state_panic" $(BUILD)/readobj.txt
	rg -q "runtime_format_parse_field_from_argument" $(BUILD)/readobj.txt
	rg -q "runtime_format_parse_decimal_field" $(BUILD)/readobj.txt
	rg -q "runtime_format_write_integer_conversion" $(BUILD)/readobj.txt
	rg -q "runtime_format_write_padded_span" $(BUILD)/readobj.txt
	rg -q "runtime_format_write_padding" $(BUILD)/readobj.txt
	rg -q "runtime_format_buffer_context_validate" $(BUILD)/readobj.txt
	rg -q "runtime_format_validate_byte_span" $(BUILD)/readobj.txt
	rg -q "runtime_format_buffer_write" $(BUILD)/readobj.txt
	rg -q "runtime_vsnprintf_chk" $(BUILD)/readobj.txt
	rg -q "runtime_vsnprintf_object_size_fatal" $(BUILD)/readobj.txt
	rg -q "runtime_fatal_vformat" $(BUILD)/readobj.txt
	rg -q "runtime_fatal_render_and_publish" $(BUILD)/readobj.txt
	rg -q "runtime_fatal_format_bounded" $(BUILD)/readobj.txt
	rg -q "runtime_abort_fatal" $(BUILD)/readobj.txt
	rg -q "runtime_fatal_nesting_enter" $(BUILD)/readobj.txt
	rg -q "runtime_fatal_outlined_prepare_format_arguments" $(BUILD)/readobj.txt
	rg -q "ccn_zero" $(BUILD)/readobj.txt
	rg -q "ccn_seti" $(BUILD)/readobj.txt
	rg -q "ccn_swap" $(BUILD)/readobj.txt
	rg -q "ccn_cmp_asm" $(BUILD)/readobj.txt
	rg -q "ccn_bitlen_public_value" $(BUILD)/readobj.txt
	rg -q "ccn_bitlen_internal" $(BUILD)/readobj.txt
	rg -q "ccn_cmp_internal" $(BUILD)/readobj.txt
	rg -q "ccn_n" $(BUILD)/readobj.txt
	rg -q "ccn_read_uint_public_value" $(BUILD)/readobj.txt
	rg -q "ccn_read_uint_internal" $(BUILD)/readobj.txt
	rg -q "ccn_set" $(BUILD)/readobj.txt
	rg -q "ccn_write_uint_size_public_value" $(BUILD)/readobj.txt
	rg -q "ccn_trailing_zeros" $(BUILD)/readobj.txt
	rg -q "ccn_n_noasm" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_zero_32_byte_span" $(BUILD)/readobj.txt
	rg -q "runtime_ascii_is_space" $(BUILD)/readobj.txt
	rg -q "runtime_ctype_flags_byte" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_memset_s_checked" $(BUILD)/readobj.txt
	rg -q "runtime_errno_span" $(BUILD)/readobj.txt
	rg -q "runtime_bounded_strtoull" $(BUILD)/readobj.txt
	rg -q "runtime_bounded_strdup" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_validate_byte_span" $(BUILD)/readobj.txt
	rg -q "runtime_bounded_strlen_source_token" $(BUILD)/readobj.txt
	rg -q "runtime_bounded_memmove_capacity_panic" $(BUILD)/readobj.txt
	rg -q "runtime_bounded_memset_capacity_panic" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_compare_span" $(BUILD)/readobj.txt
	rg -q "runtime_memset_s" $(BUILD)/readobj.txt
	rg -q "runtime_memset_s_checked" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_outlined_validate_left_span_save_right" $(BUILD)/readobj.txt
	rg -q "runtime_bzero_tail_clone" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_validate_byte_span_thunk" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_validate_span_thunk" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_validate_record_tuple" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_outlined_load_x20_span_bounds" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_outlined_restore_destination_bounds" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_validate_byte_span" $(BUILD)/readobj.txt
	rg -q "usb_configuration_descriptor_validate_tuple" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_outlined_load_source_span_x8_offset_x19" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_outlined_offset_source_current_and_stage_copy_length" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_outlined_restore_destination_span_x27" $(BUILD)/readobj.txt
	rg -q "usb_descriptor_outlined_copy_configuration_header_x21" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_builder_bounded_view" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_source_token" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_outlined_save_triplet_x21_x19" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_outlined_compare_field_descriptor_magic" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_outlined_save_field_bounds_x21_x20" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_outlined_load_ptr_is_raw_tag_x8" $(BUILD)/readobj.txt
	rg -q "runtime_rpc_outlined_load_ptr_is_not_ptr_tag_x8" $(BUILD)/readobj.txt
	rg -q "runtime_current_task_rpc_arena_slot_bounded_view" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_write" $(BUILD)/readobj.txt
	rg -q "root_rpc_memory_span_is_readable" $(BUILD)/readobj.txt
	rg -q "root_rpc_memory_outlined_stage_u64_field" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_prepare_service_call" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_stack_guard_compare_saved_x8" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_prepare_service_call" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_prepare_builder_view_and_source_current" $(BUILD)/readobj.txt
	rg -q "root_rpc_memory_outlined_save_source_span_x20_x23" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_prepare_u32_field_arguments" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_initialize_sp28_builder_arguments" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_initialize_sp20_builder_arguments" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_restore_response_span_x4_x7" $(BUILD)/readobj.txt
	rg -q "root_rpc_memory_outlined_prepare_u64_field_arguments" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_clear_sp20_builder" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_stack_guard_load_to_x8" $(BUILD)/readobj.txt
	rg -q "root_rpc_lifecycle_outlined_append_empty_field_zero" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_prepare_single_byte_field" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_sp28_builder_span_x21_with_type" $(BUILD)/readobj.txt
	rg -q "root_rpc_memory_outlined_require_response_field" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_prepare_u64_field_arguments" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_stage_u64_stack_field" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_sp28_builder_bounds_x21" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_prepare_byte_field_lengths" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_clear_sp10_and_prepare_sp20_builder" $(BUILD)/readobj.txt
	rg -q "root_rpc_lifecycle_outlined_clear_builder_frame" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_sp28_builder_span_x28_with_type" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_load_is_not_ptr_tag_x24" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_stage_byte_stack_field" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_save_builder_and_prepare_pointer_field" $(BUILD)/readobj.txt
	rg -q "root_rpc_memory_outlined_load_ptr_is_raw_tag_x7" $(BUILD)/readobj.txt
	rg -q "root_rpc_memory_outlined_prepare_32_byte_field_arguments" $(BUILD)/readobj.txt
	rg -q "root_rpc_console_outlined_load_builder_type_x23" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_prologue_save_x19_x28_allocate_c0" $(BUILD)/readobj.txt
	rg -q "root_rpc_outlined_epilogue_restore_x19_x28_release_c0" $(BUILD)/readobj.txt
	rg -q "usb_build_serial_identity" $(BUILD)/readobj.txt
	rg -q "usb_build_nonce_identity_string" $(BUILD)/readobj.txt
	rg -q "usb_identity_validate_byte_span" $(BUILD)/readobj.txt
	rg -q "usb_nonce_outlined_prepare_72_byte_output_bounds" $(BUILD)/readobj.txt
	rg -q "usb_identity_outlined_prepare_bounded_string_arguments" $(BUILD)/readobj.txt
	rg -q "usb_identity_outlined_set_destination_limit_127" $(BUILD)/readobj.txt
	rg -q "usb_identity_outlined_restore_destination_base_and_type" $(BUILD)/readobj.txt
	rg -q "usb_identity_outlined_global_string_span" $(BUILD)/readobj.txt
	rg -q "usb_get_manufacturer_string" $(BUILD)/readobj.txt
	rg -q "usb_get_product_string" $(BUILD)/readobj.txt
	rg -q "usb_compute_dfu_product_id" $(BUILD)/readobj.txt
	rg -q "usb_get_apple_vendor_id" $(BUILD)/readobj.txt
	rg -q "boot_arguments_parse_next_token" $(BUILD)/readobj.txt
	rg -q "boot_arguments_lookup_value" $(BUILD)/readobj.txt
	rg -q "boot_arguments_copy_value" $(BUILD)/readobj.txt
	rg -q "boot_arguments_get_uint64" $(BUILD)/readobj.txt
	rg -q "boot_arguments_validate_byte_span" $(BUILD)/readobj.txt
	rg -q "boot_arguments_load_platform_property" $(BUILD)/readobj.txt
	rg -q "boot_arguments_source_token" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_restore_input_span_x27_x24" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_clear_result_span_x27" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_restore_owned_span_suffix_x21_x23" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_prepare_key_length_output_bounds" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_save_input_span_x27_x24" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_save_owned_span_x27_x20" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_save_input_span_x27_x24_clone" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_restore_token_span_x20_x25" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_frame90_epilogue" $(BUILD)/readobj.txt
	rg -q "boot_arguments_outlined_frame90_prologue" $(BUILD)/readobj.txt
	rg -q "runtime_copy_from_unprivileged" $(BUILD)/readobj.txt
	rg -q "runtime_copy_to_unprivileged" $(BUILD)/readobj.txt
	rg -q "runtime_unprivileged_copy_validate_u64_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_unprivileged_copy_validate_u32_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_unprivileged_copy_validate_u16_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_unprivileged_copy_make_destination_byte_span" $(BUILD)/readobj.txt
	rg -q "runtime_unprivileged_copy_make_source_byte_span" $(BUILD)/readobj.txt
	rg -q "runtime_unprivileged_load_u64_arch" $(BUILD)/readobj.txt
	rg -q "runtime_unprivileged_store_u64_arch" $(BUILD)/readobj.txt
	rg -q "simple_virtio_bus_base" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_slot" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_mmio_base" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_endpoint_queue_initialize" $(BUILD)/readobj.txt
	rg -q "simple_virtio_block_write" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_validate_controller_tuple" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_validate_device_tuple" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_validate_queue_tuple" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_validate_buffer_tuple" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_validate_completion_tuple" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_outlined_load_device_bounds_x19" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_outlined_load_device_bounds_x25" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_outlined_load_device_bounds_x21" $(BUILD)/readobj.txt
	rg -q "usb_event_outlined_prepare_payload_dispatch_arguments" $(BUILD)/readobj.txt
	rg -q "pvpanic_probe_and_cache" $(BUILD)/readobj.txt
	rg -q "pvpanic_raise_panic_event" $(BUILD)/readobj.txt
	rg -q "runtime_trace_emit_callsite_event" $(BUILD)/readobj.txt
	rg -q "runtime_trace_append_timestamped_event" $(BUILD)/readobj.txt
	rg -q "runtime_physical_memory_source_token" $(BUILD)/readobj.txt
	rg -q "runtime_log_set_secondary_function" $(BUILD)/readobj.txt
	rg -q "runtime_dynamic_stack_allocation_probe_noop" $(BUILD)/readobj.txt
	rg -q "boot_image_stack_guard_compare_saved_x8" $(BUILD)/readobj.txt
	rg -q "runtime_stack_guard_seed_normalize" $(BUILD)/readobj.txt
	rg -q "lz4_decode_buffer" $(BUILD)/readobj.txt
	rg -q "lz4_decode_asm" $(BUILD)/readobj.txt
	rg -q "adler32" $(BUILD)/readobj.txt
	rg -q "zlib_decode_buffer" $(BUILD)/readobj.txt
	rg -q "zlibDecodeBufferSafe" $(BUILD)/readobj.txt
	rg -q "readHuffmanTable" $(BUILD)/readobj.txt
	rg -q "lz4raw_decode_buffer" $(BUILD)/readobj.txt
	rg -q "lz4_decode" $(BUILD)/readobj.txt
	rg -q "ce0384b99949b5c8e80548d3b311c39ed0d5eb7a" \
		vendor/mbedtls/PROVENANCE.md
	rg -q "Apache-2.0" vendor/mbedtls/PROVENANCE.md
	rg -q "runtime_task_require_exception_context" $(BUILD)/readobj.txt
	rg -q "runtime_log_set_function" $(BUILD)/readobj.txt
	rg -q "runtime_log_get_function" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_thunk_direct" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_thunk_spill_a" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_panic_thunk_spill_b" $(BUILD)/readobj.txt
	rg -q "runtime_domain_from_registry_link" $(BUILD)/readobj.txt
	rg -q "runtime_domain_create_and_register" $(BUILD)/readobj.txt
	rg -q "runtime_domain_identity_read" $(BUILD)/readobj.txt
	rg -q "runtime_domain_record_identity" $(BUILD)/readobj.txt
	rg -q "runtime_service_call" $(BUILD)/readobj.txt
	rg -q "runtime_service_call_from_exception" $(BUILD)/readobj.txt
	rg -q "runtime_task_activate_capability" $(BUILD)/readobj.txt
	rg -q "runtime_task_create" $(BUILD)/readobj.txt
	rg -q "runtime_task_exit" $(BUILD)/readobj.txt
	rg -q "runtime_task_join" $(BUILD)/readobj.txt
	rg -q "avp_synchronous_exception" $(BUILD)/readobj.txt
	rg -q "avp_exception_entry" $(BUILD)/readobj.txt
	rg -q "runtime_task_yield" $(BUILD)/readobj.txt
	rg -q "usb_device_handle_event" $(BUILD)/readobj.txt
	rg -q "usb_device_initialize" $(BUILD)/readobj.txt
	rg -q "simple_virtio_usb_initialize" $(BUILD)/readobj.txt
	rg -q "dfu_moded_control_xfer_cb" $(BUILD)/readobj.txt
	rg -q "tud_task_ext" $(BUILD)/readobj.txt
	! rg -q "usb_dfu_handle_event" $(BUILD)/readobj.txt
	rg -q "platform_compute_iboot_flags" $(BUILD)/readobj.txt
	rg -q "platform_sha384_digest_u64_to_span" $(BUILD)/readobj.txt
	rg -q "platform_generate_boot_nonce" $(BUILD)/readobj.txt
	rg -q "platform_status_flag5_set" $(BUILD)/readobj.txt
	rg -q "platform_boot_nonce_clear_if_requested" $(BUILD)/readobj.txt
	rg -q "platform_get_boot_nonce" $(BUILD)/readobj.txt
	rg -q "platform_identity_prepare_usb_nonces" $(BUILD)/readobj.txt
	rg -q "platform_identity_copy_boot_hardware_words" $(BUILD)/readobj.txt
	rg -q "platform_identity_set_boot_hardware_words" $(BUILD)/readobj.txt
	rg -q "platform_status_flag3_is_set" $(BUILD)/readobj.txt
	rg -q "platform_status_flag3_update" $(BUILD)/readobj.txt
	rg -q "platform_copy_variant_name" $(BUILD)/readobj.txt
	rg -q "platform_identity_validate_u32_span_thunk" $(BUILD)/readobj.txt
	rg -q "j_platform_identity_validate_u32_span" $(BUILD)/readobj.txt
	rg -q "platform_identity_validate_u32_span" $(BUILD)/readobj.txt
	rg -q "platform_identity_outlined_load_status_and_base_x19_w8" $(BUILD)/readobj.txt
	rg -q "platform_sha384_digest_bounded" $(BUILD)/readobj.txt
	rg -q "platform_sha384_digest_bounded_thunk_a" $(BUILD)/readobj.txt
	rg -q "platform_sha384_digest_bounded_thunk_b" $(BUILD)/readobj.txt
	rg -q "platform_compute_chip_production_fuse_mode" $(BUILD)/readobj.txt
	rg -q "platform_read_effective_production_mode" $(BUILD)/readobj.txt
	rg -q "j_platform_entropy_set_control_flag_bit0_thunk" $(BUILD)/readobj.txt
	rg -q "platform_entropy_read_word" $(BUILD)/readobj.txt
	rg -q "j_platform_entropy_reinitialize_fatal" $(BUILD)/readobj.txt
	rg -q "platform_entropy_primary_request_is_enabled_thunk" $(BUILD)/readobj.txt
	rg -q "platform_entropy_read_primary_data_thunk" $(BUILD)/readobj.txt
	rg -q "platform_entropy_set_control_flag_bit0_thunk" $(BUILD)/readobj.txt
	rg -q "platform_entropy_set_control_flag_bit1_thunk" $(BUILD)/readobj.txt
	rg -q "platform_entropy_primary_request_is_enabled" $(BUILD)/readobj.txt
	rg -q "platform_entropy_request_is_enabled" $(BUILD)/readobj.txt
	rg -q "platform_entropy_read_primary_data" $(BUILD)/readobj.txt
	rg -q "platform_entropy_read_request" $(BUILD)/readobj.txt
	rg -q "platform_entropy_request_has_data" $(BUILD)/readobj.txt
	rg -q "platform_entropy_write_request" $(BUILD)/readobj.txt
	rg -q "platform_entropy_set_control_flag_bit0" $(BUILD)/readobj.txt
	rg -q "platform_entropy_set_control_flag_bit1" $(BUILD)/readobj.txt
	rg -q "platform_entropy_descriptor_transport_is_enabled" $(BUILD)/readobj.txt
	rg -q "platform_entropy_request_records_equal" $(BUILD)/readobj.txt
	rg -q "platform_entropy_submit_command_descriptor" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_prepare_control_flag_request_arguments" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_prepare_primary_descriptor_result" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_compare_stack_request_with_x3_record" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_compare_x19_request_with_x3_record" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_load_primary_request_identifier" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_compare_stack_request_with_x3_record_variant" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_set_descriptor_class_and_size" $(BUILD)/readobj.txt
	rg -q "platform_entropy_load_secondary_request_record_to_x3" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_load_request_record_and_clear_flag" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_compare_x19_request_with_x3_record_tail" $(BUILD)/readobj.txt
	rg -q "platform_entropy_reinitialize_fatal" $(BUILD)/readobj.txt
	rg -q "platform_dma_cacheline_invalidate_noop" $(BUILD)/readobj.txt
	rg -q "platform_dma_cacheline_clean_noop" $(BUILD)/readobj.txt
	rg -q "platform_dma_cache_invalidate_all_bootstrap_noop" $(BUILD)/readobj.txt
	rg -q "platform_dma_cache_clean_all_noop" $(BUILD)/readobj.txt
	rg -q "j_platform_dma_cache_invalidate_all_bootstrap_noop" $(BUILD)/readobj.txt
	rg -q "platform_dma_cache_invalidate_all" $(BUILD)/readobj.txt
	rg -q "platform_dma_cache_maintain_bounded_range" $(BUILD)/readobj.txt
	rg -q "platform_dma_cache_clean_all_unlocked" $(BUILD)/readobj.txt
	rg -q "platform_publish_double_panic" $(BUILD)/readobj.txt
	rg -q "platform_panic_reset" $(BUILD)/readobj.txt
	rg -q "platform_publish_iboot_panic" $(BUILD)/readobj.txt
	rg -q "platform_panic_outlined_write_separator" $(BUILD)/readobj.txt
	rg -q "platform_entropy_enable_primary_request_thunk" $(BUILD)/readobj.txt
	rg -q "platform_entropy_seed_primary_data_thunk" $(BUILD)/readobj.txt
	rg -q "platform_entropy_enable_primary_request" $(BUILD)/readobj.txt
	rg -q "platform_entropy_seed_primary_data" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_validate_saved_qword_tuple_x19_x22" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_validate_saved_qword_tuple_x19_x23" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_prepare_stack_seed_span" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_load_request_status_register_to_w8" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_load_request_type_to_x3" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_copy_primary_request_identifier_to_stack" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_clear_write_fallback_state" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_save_request_tuple_and_stack_guard" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_control_flag_frame_epilogue" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_control_flag_frame_prologue" $(BUILD)/readobj.txt
	rg -q "platform_entropy_derive_boot_policy_outputs" $(BUILD)/readobj.txt
	rg -q "platform_entropy_policy_initialize" $(BUILD)/readobj.txt
	rg -q "platform_entropy_seed_and_enable_primary_request" $(BUILD)/readobj.txt
	rg -q "platform_entropy_return_empty_seed_span" $(BUILD)/readobj.txt
	rg -q "platform_entropy_outlined_prepare_stack_seed_span_sp8_x20" $(BUILD)/readobj.txt
	rg -q "platform_entropy_stack_guard_compare_x8" $(BUILD)/readobj.txt
	rg -q "platform_entropy_stack_guard_load_x8" $(BUILD)/readobj.txt
	rg -q "platform_outlined_load_byte_type_to_x3" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_status_skips_registration" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_get_entry" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_source_create" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_source_record_failure_clear_size" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_source_record_bounds_panic" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_validate_source_record_tuple_thunk" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_load_source_list_slots" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_lock_bounded_view" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_load_lock_type_x3" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_restore_source_record_bounds_x20_x22" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_validate_source_record_tuple" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_save_method_span_bounds_x24_x22" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_load_method_span_and_compare_lower_bound" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_load_source_list_sentinel_x10" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_validate_source_record_and_compare_lower_bound" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_restore_source_list_bounds_x19_x28" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_lock_release" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_decode_and_register_source" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_initialize_callbacks_once" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_validate_encoded_byte_span" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_source_destroy" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_source_copy_entry" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_source_get_entry_view" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_payload_entry_get_value" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_load_and_register_record" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_log_generated_location" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_noop_log_callback" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_create_source_from_decoded_config" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_save_source_span_x22_x19" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_extract_payload_entry" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_outlined_restore_source_span_x22_x19" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_noop_log_sink" $(BUILD)/readobj.txt
	rg -q "platform_syscfg_fatal_format_callback" $(BUILD)/readobj.txt
	rg -q "runtime_boot_handoff_scrub_and_enter_trampoline" $(BUILD)/readobj.txt
	rg -q "runtime_boot_handoff_install_scrub_trampoline" $(BUILD)/readobj.txt
	rg -q "runtime_boot_handoff_commit_and_enter" $(BUILD)/readobj.txt
	rg -q "runtime_boot_handoff_enter_payload" $(BUILD)/readobj.txt
	rg -q "boot_finalize_services_and_enter_payload" $(BUILD)/readobj.txt
	rg -q "runtime_boot_handoff_commit_and_enter_svc" $(BUILD)/readobj.txt
	rg -q "runtime_boot_handoff_zero_range" $(BUILD)/readobj.txt
	rg -q "runtime_boot_handoff_zero_range_failure" $(BUILD)/readobj.txt
	rg -q "runtime_boot_source_token" $(BUILD)/readobj.txt
	rg -q "Img4DecodePerformTrustEvaluationWithCallbacks" $(BUILD)/readobj.txt
	rg -q "_Img4DecodePerformTrustEvaluationWithCallbacksInternal" $(BUILD)/readobj.txt
	rg -q "img4_decode_stack_guard_compare_saved_x8" $(BUILD)/readobj.txt
	rg -q "img4_decode_stack_guard_load_to_x8" $(BUILD)/readobj.txt
	rg -q "img4_decode_stack_guard_preserve_x0_load_to_x9" $(BUILD)/readobj.txt
	rg -q "Img4DecodeCopyPayloadHashWithCallback" $(BUILD)/readobj.txt
	rg -q "Img4DecodeEvaluateCertificateProperties" $(BUILD)/readobj.txt
	rg -q "img4_decode_stack_guard_compare_saved_x8_clone" $(BUILD)/readobj.txt
	rg -q "img4_decode_property_outlined_bind_output_and_save_stack_guard" $(BUILD)/readobj.txt
	rg -q "img4_decode_stack_guard_load_to_x8_clone" $(BUILD)/readobj.txt
	rg -q "img4_decode_property_outlined_zero_decode_scratch" $(BUILD)/readobj.txt
	rg -q "img4_decode_property_outlined_decode_private_tag" $(BUILD)/readobj.txt
	rg -q "img4_decode_property_outlined_prepare_value_parse" $(BUILD)/readobj.txt
	rg -q "Img4DecodeCopyManifestHashWithCallback" $(BUILD)/readobj.txt
	rg -q "Img4DecodeEvaluateCertificatePropertiesInternal" $(BUILD)/readobj.txt
	rg -q "Img4DecodeEvaluateDictionaryProperties" $(BUILD)/readobj.txt
	rg -q "Img4DecodeComputeDigest" $(BUILD)/readobj.txt
	rg -q "verify_chain_img4_v2_with_crack_callback" $(BUILD)/readobj.txt
	rg -q "verify_chain_img4_v2" $(BUILD)/readobj.txt
	rg -q "crack_chain_rsa4k_sha384" $(BUILD)/readobj.txt
	rg -q "boot_initialize_root_firmware_services" $(BUILD)/readobj.txt
	rg -q "register_virtual_firmware_provider" $(BUILD)/readobj.txt
	rg -q "runtime_provider_register_storage_window_dispatch_bridge" $(BUILD)/readobj.txt
	rg -q "runtime_page_span_attribute_mask" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_registry_state_span" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_registry_lookup" $(BUILD)/readobj.txt
	rg -q "runtime_memory_range_page_attribute_mask" $(BUILD)/readobj.txt
	rg -q "runtime_page_span_conversion_failure" $(BUILD)/readobj.txt
	rg -q "runtime_page_span_attribute_claim" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_flags_valid_for_span" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_address_allowed" $(BUILD)/readobj.txt
	rg -q "runtime_byte_bounds_attribute_claim" $(BUILD)/readobj.txt
	rg -q "runtime_memory_range_attribute_claim" $(BUILD)/readobj.txt
	rg -q "runtime_page_span_attribute_clear" $(BUILD)/readobj.txt
	rg -q "runtime_byte_bounds_attribute_clear" $(BUILD)/readobj.txt
	rg -q "runtime_memory_range_attribute_clear" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_registry_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_record_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_registry_lower_bound_slot_span" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_registry_upper_bound_slot_span" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_policy_active_flag_bounded_view" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_policy_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_record_update_range" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_zero_optional_arguments_clone" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_registry_test_initialized" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_policy_active_flag_to_w8" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_restore_lookup_result" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_save_lookup_result_and_validate_flags" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_load_secondary_bitmap_arguments" $(BUILD)/readobj.txt
	rg -q "runtime_memory_range_attribute_claim_flags_6_10000" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_outlined_prepare_static_range_arguments" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_outlined_initialize_registry_record" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_rounded_range_size" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_zero_optional_arguments" $(BUILD)/readobj.txt
	rg -q "runtime_page_attribute_outlined_prepare_flags_6_10000" $(BUILD)/readobj.txt
	rg -q "runtime_provider_register_storage_window" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_initialize" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_slot0_forward" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_slot1_forward" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_slot2_forward" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_slot3_forward" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_slot4_forward" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_validate_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_prepare_callback_binding" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_source_token" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_backing_descriptor_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_save_secondary_tuple_x22_x19" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_duplicate_current_lower" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_restore_secondary_tuple_x22_x19" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_scale_transfer_by_element_shift" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_create" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_validate_allocated_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_window_restore_saved_primary_to_secondary" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_descriptor_implementation_dispatch" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_descriptor_implementation_is_enabled" $(BUILD)/readobj.txt
	rg -q "runtime_provider_register_typed_storage" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot0_dispatch_bridge" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_operation_dispatch" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot1_dispatch_bridge" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_read_dispatch_bridge" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_write_dispatch_bridge" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot4_dispatch_bridge" $(BUILD)/readobj.txt
	rg -q "runtime_provider_set_descriptor_alignment_within_instance_size" $(BUILD)/readobj.txt
	rg -q "runtime_provider_set_descriptor_alignment_above_instance_size" $(BUILD)/readobj.txt
	rg -q "runtime_provider_bind_callback_slot0" $(BUILD)/readobj.txt
	rg -q "runtime_provider_bind_callback_slot1" $(BUILD)/readobj.txt
	rg -q "runtime_provider_bind_callback_slot2" $(BUILD)/readobj.txt
	rg -q "runtime_provider_bind_callback_slot3" $(BUILD)/readobj.txt
	rg -q "runtime_provider_bind_callback_slot4" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_descriptor_retain_implementation" $(BUILD)/readobj.txt
	rg -q "runtime_provider_bind_read_route_dispatch_bridge" $(BUILD)/readobj.txt
	rg -q "runtime_provider_bind_write_route_dispatch_bridge" $(BUILD)/readobj.txt
	rg -q "runtime_provider_validate_descriptor_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_validate_dispatch_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_validate_implementation_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_validate_tuple_tail_clone" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_save_x0_x6_to_x26_x20_and_clear_x0" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_descriptor_tuple_x26_x23" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_capture_implementation_bounds_and_test_descriptor_enabled" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_descriptor_tuple_x22_x19" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_secondary_span_to_x4_x7" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_save_descriptor_and_binding_pair_then_resolve_implementation" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_prepare_typed_storage_operation_arguments" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_descriptor_tuple_x24_x21" $(BUILD)/readobj.txt
	rg -q "runtime_provider_validate_common_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_save_span_x24_x21" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_registration_descriptor_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_save_descriptor_tuple_x22_x19" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_frame70_restore_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_frame70_save_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_save_descriptor_tuple_and_test_enabled" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_descriptor_tuple_and_resolve_implementation" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_save_registration_descriptor_bounds_x23_x25_x24" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_compare_saved_svc62_domain_to_three" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_load_stack_guard_x8" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_compare_svc62_domain_to_three" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_prepare_16_byte_count_clone" $(BUILD)/readobj.txt
	rg -q "runtime_provider_copy_16_byte_route_value_clone" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_prepare_registration_dispatch_and_metadata" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_frame60_restore_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_frame60_save_x19_x28" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_validate_descriptor_and_dispatch_x27" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_store_descriptor_alignment_and_compute_shift" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_capture_svc62_domain_and_test_descriptor_enabled" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_provider_tuple_and_backing_current" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_compare_stack_guard_x8" $(BUILD)/readobj.txt
	rg -q "runtime_firebloom_load_byte_type_to_x7_clone" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_add_saved_context_and_create_implementation" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_save_current_and_prepare_domain3_sync" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_store_descriptor_shift_and_prepare_implementation_call" $(BUILD)/readobj.txt
	rg -q "runtime_provider_alignment_shift" $(BUILD)/readobj.txt
	rg -q "runtime_provider_bind_read_route" $(BUILD)/readobj.txt
	rg -q "runtime_provider_bind_write_route" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_compare" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot0_invoke" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_read_route" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_write_route" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot1_invoke" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_read_route_thunk" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot4_invoke" $(BUILD)/readobj.txt
	rg -q "runtime_provider_set_implementation_alignment_within_instance_size" $(BUILD)/readobj.txt
	rg -q "runtime_provider_set_implementation_alignment_above_instance_size" $(BUILD)/readobj.txt
	rg -q "runtime_provider_create_implementation" $(BUILD)/readobj.txt
	rg -q "runtime_provider_implementation_is_enabled" $(BUILD)/readobj.txt
	rg -q "runtime_provider_resolve_implementation_dispatch_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_read" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot1_unsupported" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_write" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot3_unsupported" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot4_unsupported" $(BUILD)/readobj.txt
	rg -q "runtime_provider_typed_storage_slot5_unsupported" $(BUILD)/readobj.txt
	rg -q "runtime_provider_validate_dispatch_tuple_clone" $(BUILD)/readobj.txt
	rg -q "runtime_provider_validate_byte_tuple_tail_clone_1" $(BUILD)/readobj.txt
	rg -q "runtime_provider_validate_byte_tuple_tail_clone_2" $(BUILD)/readobj.txt
	rg -q "runtime_provider_validate_registration_entry_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_common_tuple_validate_clone" $(BUILD)/readobj.txt
	rg -q "runtime_provider_store_source_location" $(BUILD)/readobj.txt
	rg -q "runtime_provider_restore_traversal_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_restore_destination_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_primary_current_lower_x26_x23" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_descriptor_bounds_x23_x22" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_descriptor_tuple_x19_x22_x21_x23" $(BUILD)/readobj.txt
	rg -q "runtime_provider_restore_nested_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_assert_failure" $(BUILD)/readobj.txt
	rg -q "runtime_provider_copy_16_byte_route_value" $(BUILD)/readobj.txt
	rg -q "runtime_provider_return_empty_common_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_make_low_bits_mask_w8" $(BUILD)/readobj.txt
	rg -q "runtime_provider_log_source_offset" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_descriptor_element_shift_x23_x22_x21_x20" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_save_descriptor_tuple_and_alignment" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_span_current_lower_x25_x27" $(BUILD)/readobj.txt
	rg -q "runtime_provider_log_source_location" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_validate_byte_tuple_x28_sp58" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_load_dispatch_context_tuple_d0" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_prepare_secondary_storage_tuple" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_load_capability_tuple_x8" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_compare_current_to_lower_and_save_x8" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_restore_primary_upper_type_x25_x21" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_frame70_restore_x19_x28_clone" $(BUILD)/readobj.txt
	rg -q "runtime_provider_outlined_frame70_save_x19_x28_clone" $(BUILD)/readobj.txt
	rg -q "runtime_provider_normalize_storage_operation_record" $(BUILD)/readobj.txt
	rg -q "runtime_provider_storage_normalization_source_token" $(BUILD)/readobj.txt
	rg -q "Name: __stack_chk_guard \(" $(BUILD)/readobj.txt
	rg -q "Name: __stack_chk_fail \(" $(BUILD)/readobj.txt
	rg -q "7002A230" $(BUILD)/readobj.txt
	rg -q "7003CA70" $(BUILD)/readobj.txt
	rg -q "7003CAD0" $(BUILD)/readobj.txt
	rg -q "7003D0A0" $(BUILD)/readobj.txt
	rg -q "7003D0A8" $(BUILD)/readobj.txt
	rg -q "7003D160" $(BUILD)/readobj.txt
	rg -q "7003D1E0" $(BUILD)/readobj.txt
	rg -q "7003D220" $(BUILD)/readobj.txt
	rg -q "7003D7C0" $(BUILD)/readobj.txt
	! rg -q "R_AARCH64_" $(BUILD)/readobj.txt
	$(R2) -q -e scr.color=false -a arm -b 64 -m 0x100000 \
		-c "s 0x100000; pd 80; q" $(ROM) > $(BUILD)/entry.txt
	rg -q "bl.*firmware_main|bl.*0x" $(BUILD)/entry.txt
	rg -q "msr cpacr_el1" $(BUILD)/entry.txt
	rg -q "mrs .*cntkctl_el1" $(BUILD)/entry.txt
	rg -q "msr cntkctl_el1" $(BUILD)/entry.txt
	$(R2) -q -e scr.color=false -c "s sym.source_image_handoff; pd 32; \
		s sym.avp_exception_entry; pd 320; q" $(ELF) > $(BUILD)/handoff-el1.txt
	rg -q "mov x0, x2" $(BUILD)/handoff-el1.txt
	rg -q "mov x1, xzr" $(BUILD)/handoff-el1.txt
	rg -q "mov x5, x3" $(BUILD)/handoff-el1.txt
	rg -q "mov x2, x5" $(BUILD)/handoff-el1.txt
	rg -q "mov w3, 1" $(BUILD)/handoff-el1.txt
	rg -q "svc 3" $(BUILD)/handoff-el1.txt
	rg -q "cmp w17, 3" $(BUILD)/handoff-el1.txt
	rg -q "mov x29, x0" $(BUILD)/handoff-el1.txt
	rg -q "mov x28, x1" $(BUILD)/handoff-el1.txt
	! rg -q "svc.*0x60" $(BUILD)/handoff-el1.txt
	rg -q "msr sctlr_el1" $(BUILD)/handoff-el1.txt
	rg -q "msr spsr_el1" $(BUILD)/handoff-el1.txt
	rg -q "msr elr_el1" $(BUILD)/handoff-el1.txt
	rg -q "eret" $(BUILD)/handoff-el1.txt
	$(R2) -q -e scr.color=false -c "s sym.runtime_mmu_initialize; pd 220; s sym.runtime_page_table_arch_write_mair_el1; pd 3; s sym.runtime_page_table_arch_write_tcr_el1; pd 3; s sym.runtime_page_table_arch_write_ttbr0_el1; pd 3; s sym.runtime_page_table_arch_write_ttbr1_el1; pd 3; s sym.runtime_page_table_arch_write_sctlr_el1; pd 3; s sym.runtime_page_table_arch_invalidate_all; pd 5; q" \
		$(ELF) > $(BUILD)/runtime-mmu.txt
	rg -q "msr mair_el1" $(BUILD)/runtime-mmu.txt
	rg -q "msr tcr_el1" $(BUILD)/runtime-mmu.txt
	rg -q "msr ttbr0_el1" $(BUILD)/runtime-mmu.txt
	rg -q "msr ttbr1_el1" $(BUILD)/runtime-mmu.txt
	rg -q "msr sctlr_el1" $(BUILD)/runtime-mmu.txt
	rg -q "tlbi vmalle1" $(BUILD)/runtime-mmu.txt
	$(R2) -q -e scr.color=false -c "s sym.avp_time_counter; pd 4; s sym.avp_time_microseconds; pd 16; s sym.avp_time_microseconds_to_ticks; pd 12; s sym.avp_time_delay_microseconds; pd 32; s sym.avp_time_wait_until_microseconds; pd 24; q" $(ELF) > $(BUILD)/time.txt
	rg -q "mrs .*cntvct_el0" $(BUILD)/time.txt
	rg -q "mrs .*cntfrq_el0" $(BUILD)/time.txt
	rg -q "udiv" $(BUILD)/time.txt
	rg -q "yield" $(BUILD)/time.txt
	$(R2) -q -e scr.color=false -c "s sym.runtime_initialize_read_only_thread_pointer; pd 4; q" $(ELF) > $(BUILD)/stack-guard.txt
	rg -q "msr tpidrro_el0" $(BUILD)/stack-guard.txt
	$(R2) -q -e scr.color=false -c "aaa; axt @ sym.__stack_chk_fail; q" $(ELF) > $(BUILD)/stack-protector.txt
	rg -q "CALL" $(BUILD)/stack-protector.txt
	$(R2) -q -e scr.color=false -c "s sym.runtime_domain_query_locality; pd 2; s sym.runtime_service_call; pd 2; s sym.runtime_service_receive; pd 2; s sym.runtime_service_complete; pd 2; s sym.runtime_idle_pump; pd 2; s sym.runtime_task_wait_until; pd 2; s sym.runtime_task_mask_interrupts; pd 8; s sym.avp_exception_entry; pd 260; q" $(ELF) > $(BUILD)/runtime-svc.txt
	rg -q "svc 0xc2" $(BUILD)/runtime-svc.txt
	rg -q "svc 0xc3" $(BUILD)/runtime-svc.txt
	rg -q "svc 0xc4" $(BUILD)/runtime-svc.txt
	rg -q "svc 0xc5" $(BUILD)/runtime-svc.txt
	rg -q "svc 0x22" $(BUILD)/runtime-svc.txt
	rg -q "svc 0x23" $(BUILD)/runtime-svc.txt
	rg -q "svc 0x20" $(BUILD)/runtime-svc.txt
	rg -q "svc 0x43" $(BUILD)/runtime-svc.txt
	rg -q "mrs .*esr_el1" $(BUILD)/runtime-svc.txt
	rg -q "stp q8, q9" $(BUILD)/runtime-svc.txt
	rg -q "ldp q14, q15" $(BUILD)/runtime-svc.txt
	rg -q "eret" $(BUILD)/runtime-svc.txt
	$(READOBJ) --symbols --relocations $(RUNTIME_IPC_PROBE_ELF) > $(BUILD)/runtime-ipc-readobj.txt
	rg -q "runtime_service_call" $(BUILD)/runtime-ipc-readobj.txt
	rg -q "runtime_service_receive" $(BUILD)/runtime-ipc-readobj.txt
	rg -q "runtime_service_complete" $(BUILD)/runtime-ipc-readobj.txt
	! rg -q "R_AARCH64_" $(BUILD)/runtime-ipc-readobj.txt
	$(R2) -q -e scr.color=false -c "s sym.runtime_task_yield; pd 2; s sym.runtime_task_create; pd 2; s sym.runtime_task_exit; pd 3; s sym.runtime_task_join; pd 3; s sym.runtime_task_arch_set_current; pd 2; s sym.avp_exception_entry; pd 440; q" $(ELF) > $(BUILD)/scheduler.txt
	rg -q "svc 0x42" $(BUILD)/scheduler.txt
	rg -q "svc 0x40" $(BUILD)/scheduler.txt
	rg -q "svc 0x44" $(BUILD)/scheduler.txt
	rg -q "svc 0x45" $(BUILD)/scheduler.txt
	rg -q "msr tpidr_el1" $(BUILD)/scheduler.txt
	rg -q "mrs .*tpidr_el1" $(BUILD)/scheduler.txt
	rg -q "paciza" $(BUILD)/scheduler.txt
	rg -q "autiza" $(BUILD)/scheduler.txt
	rg -q "tlbi aside1" $(BUILD)/scheduler.txt
	rg -q "bl.*runtime_irq_system_timer_fiq_from_exception" $(BUILD)/scheduler.txt
	rg -q "eret" $(BUILD)/scheduler.txt
	$(R2) -q -e scr.color=false -c "s sym.runtime_idle_pump; pd 2; s sym.runtime_task_wait_until; pd 2; s sym.runtime_irq_arch_controller_initialize; pd 24; s sym.runtime_irq_arch_virtual_timer_initialize; pd 40; s sym.runtime_timer_arch_initialize; pd 4; s sym.runtime_timer_arch_program; pd 16; s sym.runtime_timer_arch_interrupt_pending; pd 4; s sym.runtime_timer_arch_wait; pd 6; q" $(ELF) > $(BUILD)/runtime-timer.txt
	rg -q "svc 0x20" $(BUILD)/runtime-timer.txt
	rg -q "svc 0x43" $(BUILD)/runtime-timer.txt
	rg -q "msr cntv_tval_el0" $(BUILD)/runtime-timer.txt
	rg -q "msr cntv_ctl_el0" $(BUILD)/runtime-timer.txt
	rg -q "mrs .*isr_el1" $(BUILD)/runtime-timer.txt
	rg -q "msr icc_igrpen0_el1" $(BUILD)/runtime-timer.txt
	rg -q "wfi" $(BUILD)/runtime-timer.txt
	$(READOBJ) --symbols --relocations $(EVENT_PROBE_ELF) > $(BUILD)/runtime-event-readobj.txt
	rg -q "runtime_event_create" $(BUILD)/runtime-event-readobj.txt
	rg -q "runtime_event_wait" $(BUILD)/runtime-event-readobj.txt
	rg -q "runtime_event_set" $(BUILD)/runtime-event-readobj.txt
	rg -q "runtime_event_clear" $(BUILD)/runtime-event-readobj.txt
	! rg -q "R_AARCH64_" $(BUILD)/runtime-event-readobj.txt
	$(R2) -q -e scr.color=false -c "s sym.runtime_event_create; pd 4; s sym.runtime_event_wait; pd 2; s sym.runtime_event_set; pd 3; s sym.runtime_event_clear; pd 3; s sym.avp_exception_entry; pd 260; q" $(EVENT_PROBE_ELF) > $(BUILD)/runtime-event.txt
	rg -q "svc 0x50" $(BUILD)/runtime-event.txt
	rg -q "svc 0x51" $(BUILD)/runtime-event.txt
	rg -q "svc 0x52" $(BUILD)/runtime-event.txt
	rg -q "eret" $(BUILD)/runtime-event.txt
	test "$$(r2 -q -c 'p8 4 @ sym.avp_mailbox_load_pair+12; q' $(ELF))" = "082540a9"
	test "$$(r2 -q -c 'p8 4 @ sym.avp_mailbox_store_pair+12; q' $(ELF))" = "092900a9"
	test $$(stat -f %z $(ROM)) -le 524288
	shasum -a 256 $(ROM) > $(BUILD)/OpenAVB.sha256
	$(READOBJ) --file-headers --sections --symbols --relocations $(IBSS_ELF) > $(BUILD)/ibss-readobj.txt
	rg -q "Format: elf64-littleaarch64" $(BUILD)/ibss-readobj.txt
	rg -q "Entry: 0x7006C800" $(BUILD)/ibss-readobj.txt
	rg -q "pvpanic_trigger" $(BUILD)/ibss-readobj.txt
	rg -q "usb_device_handle_event" $(BUILD)/ibss-readobj.txt
	rg -q "dfu_moded_control_xfer_cb" $(BUILD)/ibss-readobj.txt
	rg -q "tud_task_ext" $(BUILD)/ibss-readobj.txt
	! rg -q "usb_dfu_handle_event" $(BUILD)/ibss-readobj.txt
	! rg -q "R_AARCH64_" $(BUILD)/ibss-readobj.txt
	test "$$(LC_ALL=C od -An -j 0 -N 4 -t x4 $(IBSS_IMAGE) | tr -d ' ')" = "53424953"
	test "$$(LC_ALL=C od -An -j 8 -N 4 -t u4 $(IBSS_IMAGE) | tr -d ' ')" = "$$(stat -f %z $(IBSS_IMAGE))"
	$(UV) tools/finalize_source_image.py verify $(IBSS_IMAGE)
	test $$(stat -f %z $(IBSS_IMAGE)) -le 524288
	shasum -a 256 $(IBSS_IMAGE) > $(BUILD)/SourceIBSS.sha256
	$(UV) tools/finalize_source_image.py verify $(NORMAL_BOOT_IMAGE)
	test $$(stat -f %z $(NORMAL_BOOT_IMAGE)) -le 524288
	$(UV) tools/verify_layout.py

clean:
	rm -rf $(BUILD)
