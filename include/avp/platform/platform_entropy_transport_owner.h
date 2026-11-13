#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#if __has_attribute(retain)
#define PLATFORM_ENTROPY_TRANSPORT_PUBLIC __attribute__((used, retain))
#else
#define PLATFORM_ENTROPY_TRANSPORT_PUBLIC __attribute__((used))
#endif

enum {
    PLATFORM_ENTROPY_REQUEST_DATA_SIZE = 0x40,
    PLATFORM_ENTROPY_HANDOFF_DATA_SIZE = 0x30,
    PLATFORM_ENTROPY_CONTROL_DATA_SIZE = 4,
    PLATFORM_ENTROPY_REQUEST_STATUS_HAS_DATA = 1u << 0,
    PLATFORM_ENTROPY_REQUEST_STATUS_ENABLED = 1u << 16,
};

typedef enum {
    PLATFORM_ENTROPY_DESCRIPTOR_READ = 0,
    PLATFORM_ENTROPY_DESCRIPTOR_WRITE = 1,
    PLATFORM_ENTROPY_DESCRIPTOR_IS_ENABLED = 2,
    PLATFORM_ENTROPY_DESCRIPTOR_HAS_DATA = 3,
    PLATFORM_ENTROPY_DESCRIPTOR_ENABLE = 4,
    PLATFORM_ENTROPY_DESCRIPTOR_SET_HAS_DATA = 5,
} PlatformEntropyDescriptorOperation;

typedef struct {
    uint64_t identifier_low;
    uint64_t identifier_high;
    uint64_t data_size;
    uint32_t kind;
    uint32_t reserved;
} PlatformEntropyRequestRecord;

typedef struct {
    uint8_t data[PLATFORM_ENTROPY_REQUEST_DATA_SIZE];
    uint32_t reserved;
    uint32_t status;
} PlatformEntropyRegisterSlot;

typedef struct {
    uint32_t version;
    uint32_t kind;
    uint64_t identifier_low;
    uint64_t identifier_high;
    uint32_t request_size;
    uint32_t operation;
    uint64_t data_physical_address;
    uint32_t data_size;
    uint32_t reserved;
} PlatformEntropyCommandDescriptor;

_Static_assert(sizeof(PlatformEntropyRequestRecord) == 0x20,
    "entropy request record size");
_Static_assert(sizeof(PlatformEntropyRegisterSlot) == 0x48,
    "entropy register slot size");
_Static_assert(offsetof(PlatformEntropyRegisterSlot, status) == 0x44,
    "entropy register status offset");
_Static_assert(sizeof(PlatformEntropyCommandDescriptor) == 0x30,
    "entropy command descriptor size");
_Static_assert(offsetof(PlatformEntropyCommandDescriptor, request_size) == 0x18,
    "entropy descriptor request-size offset");
_Static_assert(offsetof(PlatformEntropyCommandDescriptor,
    data_physical_address) == 0x20,
    "entropy descriptor data-address offset");

typedef uintptr_t (*PlatformEntropyTransportResolvePhysical)(
    void *context,
    const void *address
);
typedef void (*PlatformEntropyTransportSubmit)(
    void *context,
    volatile PlatformEntropyCommandDescriptor *descriptor
);
typedef void (*PlatformEntropyTransportProgress)(
    void *context,
    uint32_t progress
);

typedef struct {
    volatile uint32_t *entropy_word;
    volatile uint32_t *descriptor_capability;
    volatile PlatformEntropyRegisterSlot *primary_slot;
    volatile PlatformEntropyRegisterSlot *secondary_slot;
    volatile uint32_t *control_word;
    volatile uintptr_t *descriptor_doorbell;
    PlatformEntropyTransportResolvePhysical resolve_physical;
    PlatformEntropyTransportSubmit submit;
    PlatformEntropyTransportProgress publish_progress;
    void *context;
} PlatformEntropyRequestTransport;

extern const PlatformEntropyRequestRecord platform_entropy_primary_request;
extern const PlatformEntropyRequestRecord platform_entropy_secondary_request;
extern const PlatformEntropyRequestRecord platform_entropy_control_request;

void platform_entropy_request_transport_bind(
    PlatformEntropyRequestTransport transport
);
void platform_entropy_request_transport_reset(void);

PLATFORM_ENTROPY_TRANSPORT_PUBLIC uint32_t platform_entropy_read_word(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool
platform_entropy_descriptor_transport_is_enabled(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool platform_entropy_request_records_equal(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC void
platform_entropy_submit_command_descriptor(
    const PlatformEntropyRequestRecord *request,
    PlatformEntropyDescriptorOperation operation,
    void *data,
    size_t data_size
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool platform_entropy_request_is_enabled(
    const PlatformEntropyRequestRecord *request
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool
platform_entropy_primary_request_is_enabled(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool platform_entropy_request_has_data(
    const PlatformEntropyRequestRecord *request
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC int platform_entropy_read_request(
    const PlatformEntropyRequestRecord *request,
    void *output,
    size_t output_size
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC int platform_entropy_write_request(
    const PlatformEntropyRequestRecord *request,
    const void *input,
    size_t input_size
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC int platform_entropy_enable_request(
    const PlatformEntropyRequestRecord *request
);
void platform_entropy_materialize_primary_handoff(
    const void *input,
    size_t input_size
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC int platform_entropy_read_primary_data(
    void *output,
    size_t output_size
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC void
platform_entropy_set_control_flag_bit0(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC void
platform_entropy_set_control_flag_bit1(void);

PLATFORM_ENTROPY_TRANSPORT_PUBLIC void
j_platform_entropy_set_control_flag_bit0_thunk(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool
platform_entropy_primary_request_is_enabled_thunk(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC int
platform_entropy_read_primary_data_thunk(void *output, size_t output_size);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC void
platform_entropy_set_control_flag_bit0_thunk(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC void
platform_entropy_set_control_flag_bit1_thunk(void);

PLATFORM_ENTROPY_TRANSPORT_PUBLIC PlatformEntropyRequestRecord
platform_entropy_outlined_prepare_control_flag_request_arguments(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC PlatformEntropyRequestRecord
platform_entropy_outlined_prepare_primary_descriptor_result(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool
platform_entropy_outlined_compare_stack_request_with_x3_record(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool
platform_entropy_outlined_compare_x19_request_with_x3_record(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC PlatformEntropyRequestRecord
platform_entropy_outlined_load_primary_request_identifier(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool
platform_entropy_outlined_compare_stack_request_with_x3_record_variant(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC void
platform_entropy_outlined_set_descriptor_class_and_size(
    PlatformEntropyRequestRecord *request
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC const PlatformEntropyRequestRecord *
platform_entropy_load_secondary_request_record_to_x3(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC PlatformEntropyRequestRecord
platform_entropy_outlined_load_request_record_and_clear_flag(
    const PlatformEntropyRequestRecord *request,
    bool *flag
);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC bool
platform_entropy_outlined_compare_x19_request_with_x3_record_tail(
    const PlatformEntropyRequestRecord *left,
    const PlatformEntropyRequestRecord *right
);

PLATFORM_ENTROPY_TRANSPORT_PUBLIC _Noreturn void
platform_entropy_reinitialize_fatal(void);
PLATFORM_ENTROPY_TRANSPORT_PUBLIC _Noreturn void
j_platform_entropy_reinitialize_fatal(void);
