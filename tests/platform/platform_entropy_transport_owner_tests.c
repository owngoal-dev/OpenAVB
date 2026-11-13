#include "avp/platform/platform_entropy_transport_owner.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>


typedef struct {
    uint8_t primary[PLATFORM_ENTROPY_REQUEST_DATA_SIZE];
    uint8_t secondary[PLATFORM_ENTROPY_REQUEST_DATA_SIZE];
    uint32_t control;
    bool primary_enabled;
    bool primary_has_data;
    bool secondary_enabled;
    bool secondary_has_data;
    size_t submissions;
    PlatformEntropyCommandDescriptor last_descriptor;
    uint32_t progress;
} DescriptorModel;

static uint32_t entropy_word;
static uint32_t descriptor_capability;
static PlatformEntropyRegisterSlot primary_slot;
static PlatformEntropyRegisterSlot secondary_slot;
static uint32_t control_word;
static uintptr_t descriptor_doorbell;
static DescriptorModel model;
static jmp_buf panic_environment;
static bool panic_expected;

_Noreturn void runtime_firebloom_panic(void) {
    assert(panic_expected);
    longjmp(panic_environment, 1);
}

static uintptr_t resolve_physical(void *context, const void *address) {
    assert(context == &model);
    return (uintptr_t)address;
}

static bool descriptor_is(
    const volatile PlatformEntropyCommandDescriptor *descriptor,
    const PlatformEntropyRequestRecord *request
) {
    return descriptor->identifier_low == request->identifier_low &&
        descriptor->identifier_high == request->identifier_high &&
        descriptor->request_size == request->data_size &&
        descriptor->kind == request->kind;
}

static void descriptor_submit(
    void *context,
    volatile PlatformEntropyCommandDescriptor *descriptor
) {
    DescriptorModel *state = context;
    assert(state == &model);
    ++state->submissions;
    state->last_descriptor = *descriptor;
    uint8_t *data = (uint8_t *)(uintptr_t)
        descriptor->data_physical_address;
    uint8_t *stored = 0;
    bool *enabled = 0;
    bool *has_data = 0;
    if (descriptor_is(descriptor, &platform_entropy_primary_request)) {
        stored = state->primary;
        enabled = &state->primary_enabled;
        has_data = &state->primary_has_data;
    } else if (descriptor_is(
            descriptor, &platform_entropy_secondary_request)) {
        stored = state->secondary;
        enabled = &state->secondary_enabled;
        has_data = &state->secondary_has_data;
    } else if (descriptor_is(
            descriptor, &platform_entropy_control_request)) {
        stored = (uint8_t *)&state->control;
    } else {
        assert(false);
    }

    switch ((PlatformEntropyDescriptorOperation)descriptor->operation) {
    case PLATFORM_ENTROPY_DESCRIPTOR_READ:
        memcpy(data, stored, descriptor->data_size);
        break;
    case PLATFORM_ENTROPY_DESCRIPTOR_WRITE:
        memcpy(stored, data, descriptor->data_size);
        break;
    case PLATFORM_ENTROPY_DESCRIPTOR_IS_ENABLED:
        assert(enabled != 0 && descriptor->data_size == 1);
        data[0] = *enabled;
        break;
    case PLATFORM_ENTROPY_DESCRIPTOR_HAS_DATA:
        assert(has_data != 0 && descriptor->data_size == 1);
        data[0] = *has_data;
        break;
    case PLATFORM_ENTROPY_DESCRIPTOR_ENABLE:
        assert(enabled != 0);
        *enabled = true;
        break;
    case PLATFORM_ENTROPY_DESCRIPTOR_SET_HAS_DATA:
        /* Like the host: a written request holds nothing until this. */
        assert(descriptor->data_physical_address == 0 &&
            descriptor->data_size == 0);
        if (has_data != 0) {
            *has_data = true;
        }
        break;
    }
}

static void publish_progress(void *context, uint32_t progress) {
    assert(context == &model);
    model.progress = progress;
}

static void bind_transport(uint32_t descriptor_enabled) {
    memset(&primary_slot, 0, sizeof(primary_slot));
    memset(&secondary_slot, 0, sizeof(secondary_slot));
    memset(&model, 0, sizeof(model));
    entropy_word = UINT32_C(0x8a17c2d4);
    descriptor_capability = descriptor_enabled;
    control_word = 0;
    descriptor_doorbell = 0;
    panic_expected = false;
    platform_entropy_request_transport_bind(
        (PlatformEntropyRequestTransport){
            .entropy_word = &entropy_word,
            .descriptor_capability = &descriptor_capability,
            .primary_slot = &primary_slot,
            .secondary_slot = &secondary_slot,
            .control_word = &control_word,
            .descriptor_doorbell = &descriptor_doorbell,
            .resolve_physical = resolve_physical,
            .submit = descriptor_submit,
            .publish_progress = publish_progress,
            .context = &model,
        }
    );
}

static void test_records_and_semantic_outliners(void) {
    bind_transport(0);
    assert(platform_entropy_primary_request.identifier_low ==
        UINT64_C(0x2b49a37503201907));
    assert(platform_entropy_secondary_request.identifier_high ==
        UINT64_C(0x105a9a236dc061f3));
    assert(platform_entropy_control_request.identifier_low ==
        UINT64_C(0x12494ca4258deb21));
    assert(platform_entropy_request_records_equal(
        &platform_entropy_primary_request,
        &platform_entropy_primary_request));
    assert(!platform_entropy_request_records_equal(
        &platform_entropy_primary_request,
        &platform_entropy_secondary_request));

    PlatformEntropyRequestRecord record =
        platform_entropy_outlined_load_primary_request_identifier();
    assert(platform_entropy_outlined_compare_stack_request_with_x3_record(
        &record, &platform_entropy_primary_request));
    assert(platform_entropy_outlined_compare_x19_request_with_x3_record(
        &record, &platform_entropy_primary_request));
    assert(platform_entropy_outlined_compare_stack_request_with_x3_record_variant(
        &record, &platform_entropy_primary_request));
    assert(platform_entropy_outlined_compare_x19_request_with_x3_record_tail(
        &record, &platform_entropy_primary_request));
    assert(platform_entropy_request_records_equal(
        platform_entropy_load_secondary_request_record_to_x3(),
        &platform_entropy_secondary_request));

    record = (PlatformEntropyRequestRecord){0};
    platform_entropy_outlined_set_descriptor_class_and_size(&record);
    assert(record.data_size == PLATFORM_ENTROPY_REQUEST_DATA_SIZE);
    assert(record.kind == 2);
    record =
        platform_entropy_outlined_prepare_control_flag_request_arguments();
    assert(platform_entropy_request_records_equal(
        &record, &platform_entropy_control_request));
    record = platform_entropy_outlined_prepare_primary_descriptor_result();
    assert(platform_entropy_request_records_equal(
        &record, &platform_entropy_primary_request));
    bool flag = true;
    record = platform_entropy_outlined_load_request_record_and_clear_flag(
        &platform_entropy_primary_request,
        &flag
    );
    assert(!flag);
    assert(platform_entropy_request_records_equal(
        &record, &platform_entropy_primary_request));
}

static void test_register_transport(void) {
    bind_transport(0);
    assert(platform_entropy_read_word() == UINT32_C(0x8a17c2d4));
    assert(!platform_entropy_descriptor_transport_is_enabled());
    descriptor_capability = 1;
    assert(!platform_entropy_descriptor_transport_is_enabled());

    for (size_t index = 0; index < sizeof(primary_slot.data); ++index) {
        primary_slot.data[index] = (uint8_t)(index ^ 0x5a);
    }
    primary_slot.status = PLATFORM_ENTROPY_REQUEST_STATUS_ENABLED |
        PLATFORM_ENTROPY_REQUEST_STATUS_HAS_DATA;
    assert(platform_entropy_primary_request_is_enabled());
    assert(platform_entropy_primary_request_is_enabled_thunk());
    assert(platform_entropy_request_has_data(
        &platform_entropy_primary_request));
    uint8_t output[80];
    memset(output, 0xcc, sizeof(output));
    assert(platform_entropy_read_primary_data(output, sizeof(output)) == 0);
    for (size_t index = 0; index < PLATFORM_ENTROPY_REQUEST_DATA_SIZE;
         ++index) {
        assert(output[index] == (uint8_t)(index ^ 0x5a));
    }
    for (size_t index = PLATFORM_ENTROPY_REQUEST_DATA_SIZE;
         index < sizeof(output); ++index) {
        assert(output[index] == 0);
    }
    assert(platform_entropy_read_primary_data_thunk(
        output, PLATFORM_ENTROPY_REQUEST_DATA_SIZE) == 0);

    primary_slot.status &= ~PLATFORM_ENTROPY_REQUEST_STATUS_HAS_DATA;
    assert(platform_entropy_read_primary_data(output, sizeof(output)) == -1);

    uint8_t secondary[PLATFORM_ENTROPY_REQUEST_DATA_SIZE];
    memset(secondary, 0x73, sizeof(secondary));
    assert(platform_entropy_write_request(
        &platform_entropy_secondary_request,
        secondary,
        sizeof(secondary)) == 0);
    assert((secondary_slot.status &
            PLATFORM_ENTROPY_REQUEST_STATUS_HAS_DATA) != 0);
    assert(platform_entropy_enable_request(
        &platform_entropy_secondary_request) == 0);
    assert((secondary_slot.status &
            PLATFORM_ENTROPY_REQUEST_STATUS_ENABLED) != 0);
    memset(secondary, 0, sizeof(secondary));
    assert(platform_entropy_read_request(
        &platform_entropy_secondary_request,
        secondary,
        sizeof(secondary)) == 0);
    for (size_t index = 0; index < sizeof(secondary); ++index) {
        assert(secondary[index] == 0x73);
    }

    control_word = 0x40;
    platform_entropy_set_control_flag_bit0();
    assert(control_word == 0x41);
    assert(model.progress == 0x000a0018);
    platform_entropy_set_control_flag_bit1_thunk();
    assert(control_word == 0x43);
    assert(model.progress == 0x000a0016);
    j_platform_entropy_set_control_flag_bit0_thunk();
    assert(control_word == 0x43);
}

static void test_descriptor_transport(void) {
    bind_transport(1);
    assert(platform_entropy_descriptor_transport_is_enabled());
    for (size_t index = 0; index < sizeof(model.primary); ++index) {
        model.primary[index] = (uint8_t)(0xa0 + index);
    }
    model.primary_enabled = true;
    model.primary_has_data = true;
    assert(platform_entropy_primary_request_is_enabled());
    assert(model.last_descriptor.version == 1);
    assert(model.last_descriptor.operation ==
        PLATFORM_ENTROPY_DESCRIPTOR_IS_ENABLED);
    assert(model.last_descriptor.kind == 2);
    assert(model.last_descriptor.request_size ==
        PLATFORM_ENTROPY_REQUEST_DATA_SIZE);

    assert(platform_entropy_request_has_data(
        &platform_entropy_primary_request));
    model.primary_enabled = false;
    platform_entropy_submit_command_descriptor(
        &platform_entropy_primary_request,
        PLATFORM_ENTROPY_DESCRIPTOR_ENABLE,
        0,
        0);
    assert(model.primary_enabled);
    assert(model.last_descriptor.data_physical_address == 0);
    assert(model.last_descriptor.data_size == 0);
    uint8_t data[PLATFORM_ENTROPY_REQUEST_DATA_SIZE];
    assert(platform_entropy_read_request(
        &platform_entropy_primary_request, data, sizeof(data)) == 0);
    assert(memcmp(data, model.primary, sizeof(data)) == 0);

    memset(data, 0x19, sizeof(data));
    assert(!model.secondary_has_data);
    assert(platform_entropy_write_request(
        &platform_entropy_secondary_request, data, sizeof(data)) == 0);
    assert(memcmp(data, model.secondary, sizeof(data)) == 0);
    assert(model.last_descriptor.operation ==
        PLATFORM_ENTROPY_DESCRIPTOR_SET_HAS_DATA);
    assert(model.secondary_has_data);
    assert(platform_entropy_request_has_data(
        &platform_entropy_secondary_request));
    assert(platform_entropy_enable_request(
        &platform_entropy_secondary_request) == 0);
    assert(model.secondary_enabled);
    assert(model.last_descriptor.operation ==
        PLATFORM_ENTROPY_DESCRIPTOR_ENABLE);

    model.control = 0x80;
    platform_entropy_set_control_flag_bit0_thunk();
    platform_entropy_set_control_flag_bit1();
    assert(model.control == 0x83);
    assert(model.progress == 0x000a0016);
    assert(model.submissions == 14);

    uint8_t handoff[PLATFORM_ENTROPY_HANDOFF_DATA_SIZE];
    for (size_t index = 0; index < sizeof(handoff); ++index) {
        handoff[index] = (uint8_t)(0xd3u ^ index);
    }
    primary_slot.status = UINT32_C(0x40000000);
    platform_entropy_materialize_primary_handoff(handoff, sizeof(handoff));
    assert(memcmp(primary_slot.data, handoff, sizeof(handoff)) == 0);
    for (size_t index = sizeof(handoff); index < sizeof(primary_slot.data);
         ++index) {
        assert(primary_slot.data[index] == 0);
    }
    assert(primary_slot.status ==
        (UINT32_C(0x40000000) |
         PLATFORM_ENTROPY_REQUEST_STATUS_HAS_DATA |
         PLATFORM_ENTROPY_REQUEST_STATUS_ENABLED));
}

static void test_contract_failures_are_observable(void) {
    bind_transport(0);
    panic_expected = true;
    if (setjmp(panic_environment) == 0) {
        PlatformEntropyRequestRecord unknown = {0};
        (void)platform_entropy_request_is_enabled(&unknown);
        assert(false);
    }
    if (setjmp(panic_environment) == 0) {
        platform_entropy_reinitialize_fatal();
        assert(false);
    }
    if (setjmp(panic_environment) == 0) {
        j_platform_entropy_reinitialize_fatal();
        assert(false);
    }
    if (setjmp(panic_environment) == 0) {
        uint8_t invalid[PLATFORM_ENTROPY_HANDOFF_DATA_SIZE - 1] = {0};
        platform_entropy_materialize_primary_handoff(
            invalid,
            sizeof(invalid)
        );
        assert(false);
    }
    panic_expected = false;
}

int main(void) {
    test_records_and_semantic_outliners();
    test_register_transport();
    test_descriptor_transport();
    test_contract_failures_are_observable();
    platform_entropy_request_transport_reset();
    puts("platform entropy transport owner tests passed");
}
