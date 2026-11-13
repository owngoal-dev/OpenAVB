#include "avp/platform/platform_entropy_transport_owner.h"

#include "avp/runtime/memory/runtime_memory.h"


enum {
    PLATFORM_ENTROPY_WORD_ADDRESS = 0x00400038,
    PLATFORM_ENTROPY_DESCRIPTOR_CAPABILITY_ADDRESS = 0x004006ec,
    PLATFORM_ENTROPY_PRIMARY_SLOT_ADDRESS = 0x00600090,
    PLATFORM_ENTROPY_CONTROL_WORD_ADDRESS = 0x00600400,
    PLATFORM_ENTROPY_DESCRIPTOR_DOORBELL_ADDRESS = 0x30270000,
    PLATFORM_ENTROPY_PROGRESS_FLAG0 = 0x000a0018,
    PLATFORM_ENTROPY_PROGRESS_FLAG1 = 0x000a0016,
};

const PlatformEntropyRequestRecord platform_entropy_primary_request = {
    .identifier_low = UINT64_C(0x2b49a37503201907),
    .identifier_high = UINT64_C(0x87f84d2f812d97a7),
    .data_size = PLATFORM_ENTROPY_REQUEST_DATA_SIZE,
    .kind = 2,
};

const PlatformEntropyRequestRecord platform_entropy_secondary_request = {
    .identifier_low = UINT64_C(0x234c58078f543474),
    .identifier_high = UINT64_C(0x105a9a236dc061f3),
    .data_size = PLATFORM_ENTROPY_REQUEST_DATA_SIZE,
    .kind = 2,
};

const PlatformEntropyRequestRecord platform_entropy_control_request = {
    .identifier_low = UINT64_C(0x12494ca4258deb21),
    .identifier_high = UINT64_C(0x92fc19959e9b5b93),
    .data_size = PLATFORM_ENTROPY_CONTROL_DATA_SIZE,
    .kind = 1,
};

#if defined(__ELF__)
#define PLATFORM_ENTROPY_TRANSPORT_STATE \
    __attribute__((section(".runtime_prng.state"), used))
#else
#define PLATFORM_ENTROPY_TRANSPORT_STATE
#endif

static PlatformEntropyRequestTransport bound_transport
    PLATFORM_ENTROPY_TRANSPORT_STATE;
static bool descriptor_capability_cached PLATFORM_ENTROPY_TRANSPORT_STATE;
static bool descriptor_capability_value PLATFORM_ENTROPY_TRANSPORT_STATE;
static volatile PlatformEntropyCommandDescriptor command_descriptor
    PLATFORM_ENTROPY_TRANSPORT_STATE;

static _Noreturn void platform_entropy_transport_fatal(void) {
    runtime_firebloom_panic();
}

static uintptr_t platform_entropy_identity_physical(
    void *context,
    const void *address
) {
    (void)context;
    return (uintptr_t)address;
}

static void platform_entropy_default_submit(
    void *context,
    volatile PlatformEntropyCommandDescriptor *descriptor
) {
    (void)context;
    PlatformEntropyRequestTransport transport = bound_transport;
    if (transport.descriptor_doorbell == 0) {
        transport.descriptor_doorbell = (volatile uintptr_t *)
            PLATFORM_ENTROPY_DESCRIPTOR_DOORBELL_ADDRESS;
    }
    PlatformEntropyTransportResolvePhysical resolve =
        transport.resolve_physical;
    if (resolve == 0) {
        resolve = platform_entropy_identity_physical;
    }
    *transport.descriptor_doorbell =
        resolve(transport.context, (const void *)descriptor);
}

static void platform_entropy_default_progress(
    void *context,
    uint32_t progress
) {
    (void)context;
    (void)progress;
}

static PlatformEntropyRequestTransport platform_entropy_transport(void) {
    PlatformEntropyRequestTransport transport = bound_transport;
    if (transport.entropy_word == 0) {
        transport.entropy_word =
            (volatile uint32_t *)PLATFORM_ENTROPY_WORD_ADDRESS;
        transport.descriptor_capability =
            (volatile uint32_t *)PLATFORM_ENTROPY_DESCRIPTOR_CAPABILITY_ADDRESS;
        transport.primary_slot = (volatile PlatformEntropyRegisterSlot *)
            PLATFORM_ENTROPY_PRIMARY_SLOT_ADDRESS;
        transport.secondary_slot = transport.primary_slot + 1;
        transport.control_word =
            (volatile uint32_t *)PLATFORM_ENTROPY_CONTROL_WORD_ADDRESS;
        transport.descriptor_doorbell = (volatile uintptr_t *)
            PLATFORM_ENTROPY_DESCRIPTOR_DOORBELL_ADDRESS;
    }
    if (transport.resolve_physical == 0) {
        transport.resolve_physical = platform_entropy_identity_physical;
    }
    if (transport.submit == 0) {
        transport.submit = platform_entropy_default_submit;
    }
    if (transport.publish_progress == 0) {
        transport.publish_progress = platform_entropy_default_progress;
    }
    return transport;
}

static volatile PlatformEntropyRegisterSlot *platform_entropy_request_slot(
    const PlatformEntropyRequestTransport *transport,
    const PlatformEntropyRequestRecord *request
) {
    if (platform_entropy_request_records_equal(
            request, &platform_entropy_primary_request)) {
        return transport->primary_slot;
    }
    if (platform_entropy_request_records_equal(
            request, &platform_entropy_secondary_request)) {
        return transport->secondary_slot;
    }
    return 0;
}

static void platform_entropy_copy_from_volatile(
    uint8_t *destination,
    const volatile uint8_t *source,
    size_t size
) {
    for (size_t index = 0; index < size; ++index) {
        destination[index] = source[index];
    }
}

static void platform_entropy_copy_to_volatile(
    volatile uint8_t *destination,
    const uint8_t *source,
    size_t size
) {
    for (size_t index = 0; index < size; ++index) {
        destination[index] = source[index];
    }
}

void platform_entropy_request_transport_bind(
    PlatformEntropyRequestTransport transport
) {
    if (transport.entropy_word == 0 ||
        transport.descriptor_capability == 0 ||
        transport.primary_slot == 0 || transport.secondary_slot == 0 ||
        transport.control_word == 0 || transport.resolve_physical == 0 ||
        transport.submit == 0 || transport.publish_progress == 0) {
        platform_entropy_transport_fatal();
    }
    bound_transport = transport;
    descriptor_capability_cached = false;
    descriptor_capability_value = false;
}

void platform_entropy_request_transport_reset(void) {
    bound_transport = (PlatformEntropyRequestTransport){0};
    descriptor_capability_cached = false;
    descriptor_capability_value = false;
}

uint32_t platform_entropy_read_word(void) {
    return *platform_entropy_transport().entropy_word;
}

bool platform_entropy_descriptor_transport_is_enabled(void) {
    if (!descriptor_capability_cached) {
        descriptor_capability_value =
            (*platform_entropy_transport().descriptor_capability & 1u) != 0;
        descriptor_capability_cached = true;
    }
    return descriptor_capability_value;
}

bool platform_entropy_request_records_equal(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
) {
    if (left == 0 || right == 0) {
        platform_entropy_transport_fatal();
    }
    return left->identifier_low == right->identifier_low &&
        left->identifier_high == right->identifier_high &&
        left->data_size == right->data_size && left->kind == right->kind;
}

void platform_entropy_submit_command_descriptor(
    const PlatformEntropyRequestRecord *request,
    PlatformEntropyDescriptorOperation operation,
    void *data,
    size_t data_size
) {
    if (request == 0 || (data == 0 && data_size != 0) ||
        data_size > UINT32_MAX ||
        request->data_size > UINT32_MAX) {
        platform_entropy_transport_fatal();
    }
    PlatformEntropyRequestTransport transport = platform_entropy_transport();
    command_descriptor = (PlatformEntropyCommandDescriptor){
        .version = 1,
        .kind = request->kind,
        .identifier_low = request->identifier_low,
        .identifier_high = request->identifier_high,
        .request_size = (uint32_t)request->data_size,
        .operation = (uint32_t)operation,
        .data_physical_address = data == 0 ? 0 :
            transport.resolve_physical(transport.context, data),
        .data_size = (uint32_t)data_size,
    };
    transport.submit(transport.context, &command_descriptor);
}

static volatile PlatformEntropyRegisterSlot *
platform_entropy_require_request_slot(
    const PlatformEntropyRequestRecord *request
) {
    const PlatformEntropyRequestTransport transport =
        platform_entropy_transport();
    volatile PlatformEntropyRegisterSlot *slot =
        platform_entropy_request_slot(&transport, request);
    if (slot == 0) {
        platform_entropy_transport_fatal();
    }
    return slot;
}

static bool platform_entropy_request_status_bit(
    const PlatformEntropyRequestRecord *request,
    PlatformEntropyDescriptorOperation operation,
    uint32_t status_mask
) {
    if (platform_entropy_descriptor_transport_is_enabled()) {
        uint8_t value = 0;
        platform_entropy_submit_command_descriptor(
            request, operation, &value, sizeof(value));
        return (value & 1u) != 0;
    }
    return (platform_entropy_require_request_slot(request)->status &
            status_mask) != 0;
}

bool platform_entropy_request_is_enabled(
    const PlatformEntropyRequestRecord *request
) {
    return platform_entropy_request_status_bit(
        request,
        PLATFORM_ENTROPY_DESCRIPTOR_IS_ENABLED,
        PLATFORM_ENTROPY_REQUEST_STATUS_ENABLED
    );
}

bool platform_entropy_primary_request_is_enabled(void) {
    return platform_entropy_request_is_enabled(
        &platform_entropy_primary_request);
}

bool platform_entropy_request_has_data(
    const PlatformEntropyRequestRecord *request
) {
    return platform_entropy_request_status_bit(
        request,
        PLATFORM_ENTROPY_DESCRIPTOR_HAS_DATA,
        PLATFORM_ENTROPY_REQUEST_STATUS_HAS_DATA
    );
}

int platform_entropy_read_request(
    const PlatformEntropyRequestRecord *request,
    void *output,
    size_t output_size
) {
    if (request == 0 || output == 0 || output_size != request->data_size) {
        platform_entropy_transport_fatal();
    }
    if (platform_entropy_descriptor_transport_is_enabled()) {
        platform_entropy_submit_command_descriptor(
            request, PLATFORM_ENTROPY_DESCRIPTOR_READ, output, output_size);
        return 0;
    }
    const PlatformEntropyRequestTransport transport =
        platform_entropy_transport();
    volatile PlatformEntropyRegisterSlot *slot =
        platform_entropy_request_slot(&transport, request);
    if (slot != 0) {
        platform_entropy_copy_from_volatile(output, slot->data, output_size);
        return 0;
    }
    if (platform_entropy_request_records_equal(
            request, &platform_entropy_control_request)) {
        *(uint32_t *)output = *transport.control_word;
        return 0;
    }
    platform_entropy_transport_fatal();
}

int platform_entropy_write_request(
    const PlatformEntropyRequestRecord *request,
    const void *input,
    size_t input_size
) {
    if (request == 0 || input == 0 || input_size != request->data_size) {
        platform_entropy_transport_fatal();
    }
    if (platform_entropy_descriptor_transport_is_enabled()) {
        platform_entropy_submit_command_descriptor(
            request,
            PLATFORM_ENTROPY_DESCRIPTOR_WRITE,
            (void *)input,
            input_size
        );
        /* The descriptor twin of the MMIO path's HAS_DATA bit below. A
         * written request reads back as holding nothing until this is
         * sent: AVPBooter sends it with every non-empty write, and the
         * iOS 27 LLB asks for it (HAS_DATA) before reading the request. */
        platform_entropy_submit_command_descriptor(
            request, PLATFORM_ENTROPY_DESCRIPTOR_SET_HAS_DATA, 0, 0);
        return 0;
    }
    const PlatformEntropyRequestTransport transport =
        platform_entropy_transport();
    volatile PlatformEntropyRegisterSlot *slot =
        platform_entropy_request_slot(&transport, request);
    if (slot != 0) {
        platform_entropy_copy_to_volatile(
            slot->data, (const uint8_t *)input, input_size);
        slot->status |= PLATFORM_ENTROPY_REQUEST_STATUS_HAS_DATA;
        return 0;
    }
    if (platform_entropy_request_records_equal(
            request, &platform_entropy_control_request)) {
        *transport.control_word = *(const uint32_t *)input;
        return 0;
    }
    platform_entropy_transport_fatal();
}

int platform_entropy_enable_request(
    const PlatformEntropyRequestRecord *request
) {
    if (request == 0) {
        platform_entropy_transport_fatal();
    }
    if (platform_entropy_descriptor_transport_is_enabled()) {
        platform_entropy_submit_command_descriptor(
            request, PLATFORM_ENTROPY_DESCRIPTOR_ENABLE, 0, 0);
        return 0;
    }
    platform_entropy_require_request_slot(request)->status |=
        PLATFORM_ENTROPY_REQUEST_STATUS_ENABLED;
    return 0;
}

void platform_entropy_materialize_primary_handoff(
    const void *input,
    size_t input_size
) {
    if (input == 0 || input_size != PLATFORM_ENTROPY_HANDOFF_DATA_SIZE) {
        platform_entropy_transport_fatal();
    }
    const PlatformEntropyRequestTransport transport =
        platform_entropy_transport();
    platform_entropy_copy_to_volatile(
        transport.primary_slot->data, input, input_size);
#if defined(__aarch64__)
    /* The next-stage bootloader consumes the legacy MMIO slot directly,
     * even when this stage submitted the same request through descriptors. */
    __asm__ volatile("dmb sy" ::: "memory");
#endif
    transport.primary_slot->status |=
        PLATFORM_ENTROPY_REQUEST_STATUS_HAS_DATA |
        PLATFORM_ENTROPY_REQUEST_STATUS_ENABLED;
}

int platform_entropy_read_primary_data(void *output, size_t output_size) {
    if (output == 0 || (output_size & 3u) != 0) {
        platform_entropy_transport_fatal();
    }
    uint8_t data[PLATFORM_ENTROPY_REQUEST_DATA_SIZE];
    if (platform_entropy_read_request(
            &platform_entropy_primary_request, data, sizeof(data)) != 0 ||
        !platform_entropy_request_has_data(&platform_entropy_primary_request)) {
        return -1;
    }
    size_t copied = output_size;
    if (copied > sizeof(data)) {
        copied = sizeof(data);
    }
    uint8_t *bytes = output;
    for (size_t index = 0; index < copied; ++index) {
        bytes[index] = data[index];
    }
    for (size_t index = copied; index < output_size; ++index) {
        bytes[index] = 0;
    }
    return 0;
}

static void platform_entropy_set_control_flag(
    uint32_t flag,
    uint32_t progress
) {
    uint32_t control = 0;
    (void)platform_entropy_read_request(
        &platform_entropy_control_request, &control, sizeof(control));
    control |= flag;
    (void)platform_entropy_write_request(
        &platform_entropy_control_request, &control, sizeof(control));
    PlatformEntropyRequestTransport transport = platform_entropy_transport();
    transport.publish_progress(transport.context, progress);
}

void platform_entropy_set_control_flag_bit0(void) {
    platform_entropy_set_control_flag(1u, PLATFORM_ENTROPY_PROGRESS_FLAG0);
}

void platform_entropy_set_control_flag_bit1(void) {
    platform_entropy_set_control_flag(2u, PLATFORM_ENTROPY_PROGRESS_FLAG1);
}

void j_platform_entropy_set_control_flag_bit0_thunk(void) {
    platform_entropy_set_control_flag_bit0();
}

bool platform_entropy_primary_request_is_enabled_thunk(void) {
    return platform_entropy_primary_request_is_enabled();
}

int platform_entropy_read_primary_data_thunk(
    void *output,
    size_t output_size
) {
    return platform_entropy_read_primary_data(output, output_size);
}

void platform_entropy_set_control_flag_bit0_thunk(void) {
    platform_entropy_set_control_flag_bit0();
}

void platform_entropy_set_control_flag_bit1_thunk(void) {
    platform_entropy_set_control_flag_bit1();
}

PlatformEntropyRequestRecord
platform_entropy_outlined_prepare_control_flag_request_arguments(void) {
    return platform_entropy_control_request;
}

PlatformEntropyRequestRecord
platform_entropy_outlined_prepare_primary_descriptor_result(void) {
    return platform_entropy_primary_request;
}

bool platform_entropy_outlined_compare_stack_request_with_x3_record(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
) {
    return platform_entropy_request_records_equal(left, right);
}

bool platform_entropy_outlined_compare_x19_request_with_x3_record(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
) {
    return platform_entropy_request_records_equal(left, right);
}

PlatformEntropyRequestRecord
platform_entropy_outlined_load_primary_request_identifier(void) {
    return platform_entropy_primary_request;
}

bool platform_entropy_outlined_compare_stack_request_with_x3_record_variant(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
) {
    return platform_entropy_request_records_equal(left, right);
}

void platform_entropy_outlined_set_descriptor_class_and_size(
    PlatformEntropyRequestRecord *request
) {
    if (request == 0) {
        platform_entropy_transport_fatal();
    }
    request->data_size = PLATFORM_ENTROPY_REQUEST_DATA_SIZE;
    request->kind = 2;
}

const PlatformEntropyRequestRecord *
platform_entropy_load_secondary_request_record_to_x3(void) {
    return &platform_entropy_secondary_request;
}

PlatformEntropyRequestRecord
platform_entropy_outlined_load_request_record_and_clear_flag(
    const PlatformEntropyRequestRecord *request,
    bool *flag
) {
    if (request == 0 || flag == 0) {
        platform_entropy_transport_fatal();
    }
    *flag = false;
    return *request;
}

bool platform_entropy_outlined_compare_x19_request_with_x3_record_tail(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
) {
    return platform_entropy_request_records_equal(left, right);
}

_Noreturn void platform_entropy_reinitialize_fatal(void) {
    platform_entropy_transport_fatal();
}

_Noreturn void j_platform_entropy_reinitialize_fatal(void) {
    platform_entropy_reinitialize_fatal();
}
