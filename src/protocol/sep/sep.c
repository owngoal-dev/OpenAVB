#include "avp/protocol/sep/mailbox.h"
#include "avp/platform/mmio.h"
#include "avp/runtime/exception/pvpanic.h"
#include "avp/runtime/ipc/runtime_registry.h"
#include "avp/protocol/sep/sep.h"
#include "avp/protocol/sep/sep_protocol.h"
#include "avp/runtime/support/time.h"


enum {
    SEP_MAILBOX_BASE = 0x30250000,
    SEP_MAILBOX_OWNED = 1u << 31,
    SEP_MAILBOX_CHANNEL_MAGIC = 0x1111,
    EARLY_CONSOLE_DATA_IPA = 0x20010000,
    SEP_TIMEOUT_RECOVERY_LIMIT = 5,
    SEP_TIMEOUT_POLL_INTERVAL_US = 100,
    SEP_FINALIZE_ROUNDS = 8,
    SEP_READY_TIMEOUT_US = 5000000,
};

static void early_console_write(const char *message) {
    volatile uint32_t *const data =
        (volatile uint32_t *)EARLY_CONSOLE_DATA_IPA;
    while (*message != '\0') {
        *data = (uint8_t)*message++;
    }
}

static uint32_t timeout_recovery_count;
static bool preflight_wait_complete;
static uint8_t last_protocol_opcode;

/*
 * Request the slot until it reads as owned, setting the enable bit at +0x10
 * before each request as Apple's ROM does, and as the SEP ROM does on its
 * side.
 */
static void sep_acquire(void) {
    uint32_t active_control = mmio_read32(SEP_MAILBOX_BASE + 0x14);
    while ((active_control & SEP_MAILBOX_OWNED) == 0) {
        /* The ownership register acknowledges writes, so an owned slot is
         * observed and preserved without issuing another ownership request. */
        mmio_write32(
            SEP_MAILBOX_BASE + 0x10,
            mmio_read32(SEP_MAILBOX_BASE + 0x10) | 1
        );
        mmio_write32(
            SEP_MAILBOX_BASE + 0x14,
            active_control | SEP_MAILBOX_OWNED
        );
        active_control = mmio_read32(SEP_MAILBOX_BASE + 0x14);
    }
    mmio_write32(
        SEP_MAILBOX_BASE + 0x18,
        SEP_MAILBOX_CHANNEL_MAGIC
    );
    avp_memory_barrier();
}

void sep_initialize(void) {
    sep_acquire();
    /*
     * Do not send before the SEP has announced itself. After a cold start it
     * is up long before this ROM gets here, and its first message is already
     * waiting. After a warm reset (a guest reboot) it restarts together with
     * this ROM and is slower: a request sent now is dropped by the mailbox,
     * and the SEP's own setup then clears the ownership taken above, so both
     * sides wait for each other. Wait for its message instead, taking the
     * slot back when its restart drops it.
     */
    const uint64_t start = avp_time_counter();
    const uint64_t limit = avp_time_microseconds_to_ticks(SEP_READY_TIMEOUT_US);
    while ((mmio_read32(SEP_MAILBOX_BASE + 0x1c) & 0x00020000) != 0) {
        if ((mmio_read32(SEP_MAILBOX_BASE + 0x14) & SEP_MAILBOX_OWNED) == 0) {
            sep_acquire();
        }
        if (avp_time_has_elapsed(start, limit)) {
            break;
        }
    }
}

static int32_t sep_receive(
    uint32_t timeout_microseconds,
    SEPMailboxMessage *message
) {
    bool reported_empty = false;
    const uint64_t start = avp_time_counter();
    const uint64_t duration = avp_time_microseconds_to_ticks(
        timeout_microseconds
    );
    while ((mmio_read32(SEP_MAILBOX_BASE + 0x1c) & 0x00020000) != 0) {
        if (!reported_empty) {
            early_console_write("OpenBootROM: SEP RX empty\n");
            reported_empty = true;
        }
        if (timeout_microseconds == 0 ||
            avp_time_has_elapsed(start, duration)) {
            early_console_write("OpenBootROM: SEP RX timeout\n");
            return SEP_RESPONSE_WAIT_TIMEOUT;
        }
    }
    early_console_write("OpenBootROM: SEP RX ready\n");
    avp_mailbox_load_pair(
        SEP_MAILBOX_BASE + 0x100,
        &message->low,
        &message->high
    );
    return SEP_RESPONSE_WAIT_SUCCESS;
}

void sep_send(SEPMailboxMessage message) {
    while ((mmio_read32(SEP_MAILBOX_BASE + 0x20) & 0x00020000) == 0) {
    }
    avp_mailbox_store_pair(
        SEP_MAILBOX_BASE + 0x200,
        message.low,
        message.high
    );
}

static int32_t wait_for_response(
    SEPCommandDescriptor descriptor,
    uint32_t timeout_microseconds,
    SEPMailboxMessage *response
) {
    for (;;) {
        SEPMailboxMessage message;
        const int32_t receive_result = sep_receive(
            timeout_microseconds,
            &message
        );
        if (!sep_response_wait_succeeded(receive_result)) {
            return receive_result;
        }
        const SEPResponseDisposition disposition =
            sep_protocol_classify_response(descriptor, message);
        if (disposition == SEP_RESPONSE_ASYNCHRONOUS) {
            continue;
        }
        if (disposition == SEP_RESPONSE_MATCHED) {
            last_protocol_opcode = sep_protocol_opcode(message);
            *response = message;
            return SEP_RESPONSE_WAIT_SUCCESS;
        }
        if (disposition == SEP_RESPONSE_COMPLETED) {
            return SEP_RESPONSE_WAIT_SUCCESS;
        }
        return SEP_RESPONSE_WAIT_INVALID;
    }
}

static bool recover_timed_out_response(
    SEPCommandDescriptor descriptor,
    SEPMailboxMessage *response
) {
    const uint32_t previous_recovery_count = timeout_recovery_count++;
    if (previous_recovery_count >= SEP_TIMEOUT_RECOVERY_LIMIT) {
        return false;
    }

    uint32_t remaining = descriptor.timeout +
        SEP_TIMEOUT_POLL_INTERVAL_US;
    for (;;) {
        const int32_t result = wait_for_response(descriptor, 0, response);
        if (sep_response_wait_succeeded(result)) {
            return true;
        }
        if (!sep_response_wait_can_retry(result)) {
            return false;
        }
        avp_time_delay_microseconds(SEP_TIMEOUT_POLL_INTERVAL_US);
        remaining -= SEP_TIMEOUT_POLL_INTERVAL_US;
        if (remaining < SEP_TIMEOUT_POLL_INTERVAL_US + 1) {
            return false;
        }
    }
}

static bool perform_preflight_wait(SEPCommandDescriptor descriptor) {
    if (descriptor.requires_preflight_wait == 0 ||
        preflight_wait_complete) {
        return true;
    }

    uint64_t deadline;
    if (!runtime_registry_read_u64(
            AVP_RUNTIME_KEY_SEP_PREFLIGHT_DEADLINE,
            AVP_RUNTIME_TYPE_ABSOLUTE_MICROSECOND_DEADLINE,
            &deadline
        )) {
        return false;
    }
    avp_time_wait_until_microseconds(deadline);
    preflight_wait_complete = true;
    return true;
}

bool sep_execute_protocol_command(
    uint8_t command_index,
    uint8_t initial_index,
    uint32_t payload,
    uint64_t extended_payload,
    uint32_t *responses,
    uint32_t response_capacity
) {
    SEPCommandDescriptor descriptor;
    if (!sep_protocol_get_descriptor(command_index, &descriptor)) {
        return false;
    }
    const uint32_t response_count = sep_protocol_response_count(descriptor);
    if (responses != 0 && response_capacity != response_count) {
        return false;
    }
    for (uint32_t iteration = 0; iteration < response_count; ++iteration) {
        if (!perform_preflight_wait(descriptor)) {
            return false;
        }
        const uint8_t index = responses == 0
            ? initial_index
            : (uint8_t)(descriptor.response_index_stride * iteration);
        const SEPMailboxMessage request = sep_protocol_request(
            descriptor,
            index,
            payload,
            extended_payload
        );
        /* Lower synchronous opcodes leave Apple's shared slot as the request. */
        last_protocol_opcode = descriptor.command;
        sep_send(request);
        SEPMailboxMessage response = request;
        if (sep_protocol_requires_response(descriptor)) {
            const int32_t result = wait_for_response(
                descriptor,
                descriptor.timeout,
                &response
            );
            if (!sep_response_wait_succeeded(result) &&
                (!sep_response_wait_can_retry(result) ||
                 !recover_timed_out_response(descriptor, &response))) {
                return false;
            }
        }
        if (responses != 0) {
            responses[iteration] = sep_protocol_payload(response);
        }
    }
    return true;
}

SEPBootstrapResult sep_bootstrap(void) {
    sep_initialize();
    SEPBootstrapResult result = {};
    const uint32_t configuration_word_count =
        sizeof(result.configuration_words) / sizeof(uint32_t);
    if (!sep_execute_protocol_command(
            SEP_COMMAND_BOOTSTRAP,
            0,
            0,
            0,
            &result.bootstrap_payload,
            1
        ) || result.bootstrap_payload != 0xa0) {
        pvpanic_trigger();
    }

    if (!sep_execute_protocol_command(
            SEP_COMMAND_CONFIGURATION,
            0,
            0,
            0,
            result.configuration_words,
            configuration_word_count
        )) {
        pvpanic_trigger();
    }
    uint32_t configuration_or = 0;
    for (uint32_t index = 0; index < configuration_word_count; ++index) {
        configuration_or |= result.configuration_words[index];
    }
    if (configuration_or == 0) {
        pvpanic_trigger();
    }

    return result;
}

SEPBootstrapResult sep_bootstrap_with_finalize(void) {
    SEPBootstrapResult result = sep_bootstrap();
    sep_finalize_bootstrap(&result);
    return result;
}

void sep_finalize_bootstrap(SEPBootstrapResult *result) {
    SEPCommandDescriptor finalize_descriptor;
    if (!sep_protocol_get_descriptor(
            SEP_COMMAND_FINALIZE,
            &finalize_descriptor
        )) {
        pvpanic_trigger();
    }
    /* Apple's wrapper proves readiness through eight matching handshakes. */
    for (uint32_t round = 0; round < SEP_FINALIZE_ROUNDS; ++round) {
        if (!sep_execute_protocol_command(
                SEP_COMMAND_FINALIZE,
                0,
                0,
                0,
                0,
                0
            ) || last_protocol_opcode != finalize_descriptor.response) {
            pvpanic_trigger();
        }
        result->finalize_rounds = round + 1;
    }
    result->finalize_response_opcode = last_protocol_opcode;
}
