/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_leader_sequences

#include <device.h>
#include <drivers/behavior.h>
#include <logging/log.h>
#include <sys/dlist.h>
#include <kernel.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/leader.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

bool leader_status;
int32_t count = 0;
int32_t active_leader_position;

struct leader_seq_cfg {
    int32_t key_positions[CONFIG_ZMK_LEADER_MAX_KEYS_PER_SEQUENCE];
    int32_t key_position_len;
    int32_t timeout_ms;
    // if slow release is set, the combo releases when the last key is released.
    // otherwise, the combo releases when the first key is released.
    bool slow_release;
    // the virtual key position is a key position outside the range used by the keyboard.
    // it is necessary so hold-taps can uniquely identify a behavior.
    int32_t virtual_key_position;
    struct zmk_behavior_binding behavior;
    int32_t layers_len;
    int8_t layers[];
};

struct leader_seq_candidate {
    struct leader_seq_cfg *sequence;
    const zmk_event_t *key_positions_pressed[CONFIG_ZMK_LEADER_MAX_KEYS_PER_SEQUENCE];
};

// set of keys pressed
uint32_t current_sequence[CONFIG_ZMK_LEADER_MAX_KEYS_PER_SEQUENCE] = {-1};
// the set of candidate leader based on the currently pressed_keys
struct leader_seq_candidate sequence_candidates[CONFIG_ZMK_LEADER_MAX_SEQUENCES_PER_KEY];
// a lookup dict that maps a key position to all sequences on that position
struct leader_seq_cfg *sequence_lookup[ZMK_KEYMAP_LEN][CONFIG_ZMK_LEADER_MAX_SEQUENCES_PER_KEY] = {
    NULL};

// Store the leader key pointer in the leader array, one pointer for each key position
// The leader are sorted shortest-first, then by virtual-key-position.
static int intitialiaze_leader_sequences(struct leader_seq_cfg *seq) {
    for (int i = 0; i < seq->key_position_len; i++) {
        int32_t position = seq->key_positions[i];
        if (position >= ZMK_KEYMAP_LEN) {
            LOG_ERR("Unable to initialize leader, key position %d does not exist", position);
            return -EINVAL;
        }

        struct leader_seq_cfg *new_seq = seq;
        bool set = false;
        for (int j = 0; j < CONFIG_ZMK_LEADER_MAX_SEQUENCES_PER_KEY; j++) {
            struct leader_seq_cfg *sequence_at_j = sequence_lookup[position][j];
            if (sequence_at_j == NULL) {
                sequence_lookup[position][j] = new_seq;
                set = true;
                break;
            }
            if (sequence_at_j->key_position_len < new_seq->key_position_len ||
                (sequence_at_j->key_position_len == new_seq->key_position_len &&
                 sequence_at_j->virtual_key_position < new_seq->virtual_key_position)) {
                continue;
            }
            // put new_seq in this spot, move all other leader up.
            sequence_lookup[position][j] = new_seq;
            new_seq = sequence_at_j;
        }
        if (!set) {
            LOG_ERR(
                "Too many leader for key position %d, CONFIG_ZMK_LEADER_MAX_SEQUENCES_PER_KEY %d.",
                position, CONFIG_ZMK_LEADER_MAX_SEQUENCES_PER_KEY);
            return -ENOMEM;
        }
    }
    return 0;
}

static bool sequence_active_on_layer(struct leader_seq_cfg *sequence, uint8_t layer) {
    if (sequence->layers[0] == -1) {
        // -1 in the first layer position is global layer scope
        LOG_DBG("LEADER ACTIVE ON %d", layer);
        return true;
    }
    for (int j = 0; j < sequence->layers_len; j++) {
        LOG_DBG("LEADER CHECKING LAYER %d", sequence->layers[j]);
        if (sequence->layers[j] == layer) {
            LOG_DBG("LEADER ACTIVE ON %d", layer);
            return true;
        }
    }
    return false;
}

static bool has_current_sequence(struct leader_seq_cfg *sequence) {
    for (int i = 0; i < count; i++) {
        if (sequence->key_positions[i] != current_sequence[i]) {
            return false;
        }
    }
    return true;
}

static int leader_find_candidates(int32_t position) {
    LOG_DBG("LEADER FINDING CANDIDATES");
    int number_of_leader_seq_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();
    for (int i = 0; i < CONFIG_ZMK_LEADER_MAX_SEQUENCES_PER_KEY; i++) {
        struct leader_seq_cfg *sequence = sequence_lookup[position][i];
        if (sequence == NULL) {
            LOG_DBG("LEADER FOUND %d CANDIDATES", number_of_leader_seq_candidates);
            return number_of_leader_seq_candidates;
        }
        if (sequence_active_on_layer(sequence, highest_active_layer) &&
            sequence->key_positions[count] == position && has_current_sequence(sequence)) {
            LOG_DBG("FOUND CANDIDATE %d: %d", number_of_leader_seq_candidates, position);
            sequence_candidates[number_of_leader_seq_candidates].sequence = sequence;
            number_of_leader_seq_candidates++;
        }
    }
    LOG_DBG("LEADER FOUND %d CANDIDATES", number_of_leader_seq_candidates);
    return number_of_leader_seq_candidates;
}

static int clear_candidates() {
    for (int i = 0; i < CONFIG_ZMK_LEADER_MAX_SEQUENCES_PER_KEY; i++) {
        if (sequence_candidates[i].sequence == NULL) {
            return i;
        }
        sequence_candidates[i].sequence = NULL;
    }
    return CONFIG_ZMK_LEADER_MAX_SEQUENCES_PER_KEY;
}

const struct zmk_listener zmk_listener_leader;

static inline int press_leader_behavior(struct leader_seq_cfg *sequence, int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = sequence->virtual_key_position,
        .timestamp = timestamp,
    };

    return behavior_keymap_binding_pressed(&sequence->behavior, event);
}

static inline int release_leader_behavior(struct leader_seq_cfg *sequence, int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = sequence->virtual_key_position,
        .timestamp = timestamp,
    };

    return behavior_keymap_binding_released(&sequence->behavior, event);
}

void zmk_leader_activate(uint32_t position) {
    LOG_DBG("leader key activated");
    leader_status = true;
    count = 0;
    active_leader_position = position;
};

void zmk_leader_deactivate() {
    LOG_DBG("leader key deactivated");
    leader_status = false;
    clear_candidates();
};

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return 0;
    }

    if (leader_status && data->position != active_leader_position) {
        int num_candidates;
        num_candidates = leader_find_candidates(data->position);
        if (num_candidates == 0) {
            zmk_leader_deactivate();
            return 0;
        }
        if (data->state) { // keydown
            struct leader_seq_cfg *candidate_sequence = sequence_candidates[0].sequence;
            current_sequence[count] = data->position;
            press_leader_behavior(candidate_sequence, data->timestamp);
            return ZMK_EV_EVENT_HANDLED;
        } else { // keyup
            struct leader_seq_cfg *candidate_sequence = sequence_candidates[0].sequence;
            release_leader_behavior(candidate_sequence, data->timestamp);
            if (num_candidates == 1) {
                zmk_leader_deactivate();
            }
            count++;
            return ZMK_EV_EVENT_HANDLED;
        }
        return ZMK_EV_EVENT_HANDLED;
    }

    return 0;
}

ZMK_LISTENER(leader, position_state_changed_listener);
ZMK_SUBSCRIPTION(leader, zmk_position_state_changed);

#define LEADER_INST(n)                                                                             \
    static struct leader_seq_cfg sequence_config_##n = {                                           \
        .timeout_ms = 200,                                                                         \
        .virtual_key_position = ZMK_KEYMAP_LEN + __COUNTER__,                                      \
        .slow_release = false,                                                                     \
        .key_positions = DT_PROP(n, key_positions),                                                \
        .key_position_len = DT_PROP_LEN(n, key_positions),                                         \
        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                                              \
        .layers = DT_PROP(n, layers),                                                              \
        .layers_len = DT_PROP_LEN(n, layers),                                                      \
    };

DT_INST_FOREACH_CHILD(0, LEADER_INST)

#define INTITIALIAZE_LEADER_SEQUENCES(n) intitialiaze_leader_sequences(&sequence_config_##n);

static int leader_init() {
    DT_INST_FOREACH_CHILD(0, INTITIALIAZE_LEADER_SEQUENCES);
    return 0;
}

SYS_INIT(leader_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
