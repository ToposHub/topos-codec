/* RD4-07：scalable base 策略选择与 capability 记录。 */
#ifndef TOPOS_INTERNAL_BASE_POLICY_H
#define TOPOS_INTERNAL_BASE_POLICY_H

#include <stdint.h>

#include "topos_codec.h"

typedef enum tc_base_policy {
    TC_BASE_POLICY_BAND_ONLY = 0,
    TC_BASE_POLICY_SCALABLE_FAST = 1,
    TC_BASE_POLICY_SIZE_GUARDED = 2
} tc_base_policy;

typedef enum tc_base_rollout_stage {
    TC_BASE_ROLLOUT_EXPERIMENTAL = 0,
    TC_BASE_ROLLOUT_OPT_IN = 1,
    TC_BASE_ROLLOUT_DEFAULT = 2
} tc_base_rollout_stage;

/* A policy may select scalable output only when the public writer/reader and
 * the requested release gates are explicitly supplied by the caller. */
#define TC_BASE_GATE_PUBLIC_V7B_WRITER (1u << 0)
#define TC_BASE_GATE_SIZE_PASS         (1u << 1)
#define TC_BASE_GATE_SPEED_PASS        (1u << 2)

typedef enum tc_base_policy_fallback {
    TC_BASE_FALLBACK_NONE = 0,
    TC_BASE_FALLBACK_EXPERIMENTAL = 1,
    TC_BASE_FALLBACK_NO_PUBLIC_WRITER = 2,
    TC_BASE_FALLBACK_RELEASE_GATE = 3,
    TC_BASE_FALLBACK_SIZE_HINT = 4
} tc_base_policy_fallback;

typedef enum tc_base_layout_capability {
    TC_BASE_LAYOUT_BAND_ONLY = 0,
    TC_BASE_LAYOUT_BASE_ONLY_PACKET = 1
} tc_base_layout_capability;

typedef struct tc_base_policy_decision {
    uint32_t struct_size;
    uint32_t abi_version;
    uint8_t requested_policy;
    uint8_t selected_policy;
    uint8_t base_enabled;
    uint8_t layout_capability;
    uint16_t base_width;
    uint16_t base_height;
    uint8_t rollout_stage;
    uint8_t fallback_reason;
    uint16_t reserved16;
    uint32_t reserved[4];
} tc_base_policy_decision;

/* Select a deterministic policy with an explicit rollout/capability gate.
 * A zero max_dim is invalid. The public V7-B writer bit is intentionally
 * separate from the internal base-only packet primitive. */
int32_t tc_base_policy_choose_gated(const topos_frame_config* config,
                                    uint32_t max_dim, tc_base_policy requested,
                                    uint32_t scalable_hint, uint32_t band_only_hint,
                                    tc_base_rollout_stage rollout_stage,
                                    uint32_t gate_flags,
                                    tc_base_policy_decision* out);

/* Safe compatibility wrapper: the current build is experimental and has no
 * public V7-B writer, so it can never silently enable scalable output. */
int32_t tc_base_policy_choose(const topos_frame_config* config,
                              uint32_t max_dim, tc_base_policy requested,
                              uint32_t scalable_hint, uint32_t band_only_hint,
                              tc_base_policy_decision* out);

#endif /* TOPOS_INTERNAL_BASE_POLICY_H */
