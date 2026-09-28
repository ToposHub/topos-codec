#include "base_policy.h"

#include <string.h>

#include "../common/error.h"
#include "../transform/base_scale.h"

int32_t tc_base_policy_choose_gated(const topos_frame_config* config,
                                    uint32_t max_dim, tc_base_policy requested,
                                    uint32_t scalable_hint, uint32_t band_only_hint,
                                    tc_base_rollout_stage rollout_stage,
                                    uint32_t gate_flags,
                                    tc_base_policy_decision* out)
{
    if (out == NULL || config == NULL || max_dim == 0u ||
        requested > TC_BASE_POLICY_SIZE_GUARDED ||
        rollout_stage > TC_BASE_ROLLOUT_DEFAULT ||
        (gate_flags & ~(TC_BASE_GATE_PUBLIC_V7B_WRITER |
                        TC_BASE_GATE_SIZE_PASS | TC_BASE_GATE_SPEED_PASS)) != 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base policy arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = tc_frame_config_validate(config);
    if (rc != TC_OK) { return rc; }
    uint32_t base_w = 0u, base_h = 0u;
    if (!tc_base_scale_dimensions(config->visible_width, config->visible_height,
                                  max_dim, &base_w, &base_h) ||
        base_w > UINT16_MAX || base_h > UINT16_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base policy geometry");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    const uint32_t long_edge = config->visible_width > config->visible_height
                                   ? config->visible_width : config->visible_height;
    tc_base_policy selected = requested;
    tc_base_policy_fallback fallback = TC_BASE_FALLBACK_NONE;
    if (long_edge <= max_dim || long_edge <= 4096u) {
        selected = TC_BASE_POLICY_BAND_ONLY;
    } else if (requested == TC_BASE_POLICY_BAND_ONLY) {
        selected = TC_BASE_POLICY_BAND_ONLY;
    } else if (rollout_stage == TC_BASE_ROLLOUT_EXPERIMENTAL) {
        selected = TC_BASE_POLICY_BAND_ONLY;
        fallback = TC_BASE_FALLBACK_EXPERIMENTAL;
    } else if ((gate_flags & TC_BASE_GATE_PUBLIC_V7B_WRITER) == 0u) {
        selected = TC_BASE_POLICY_BAND_ONLY;
        fallback = TC_BASE_FALLBACK_NO_PUBLIC_WRITER;
    } else if (rollout_stage == TC_BASE_ROLLOUT_DEFAULT &&
               (gate_flags & (TC_BASE_GATE_SIZE_PASS | TC_BASE_GATE_SPEED_PASS)) !=
                   (TC_BASE_GATE_SIZE_PASS | TC_BASE_GATE_SPEED_PASS)) {
        selected = TC_BASE_POLICY_BAND_ONLY;
        fallback = TC_BASE_FALLBACK_RELEASE_GATE;
    } else if (requested == TC_BASE_POLICY_SIZE_GUARDED) {
        if (scalable_hint == 0u || band_only_hint == 0u || scalable_hint > band_only_hint) {
            selected = TC_BASE_POLICY_BAND_ONLY;
            fallback = TC_BASE_FALLBACK_SIZE_HINT;
        } else {
            selected = TC_BASE_POLICY_SIZE_GUARDED;
        }
    } else {
        selected = TC_BASE_POLICY_SCALABLE_FAST;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    out->requested_policy = (uint8_t)requested;
    out->selected_policy = (uint8_t)selected;
    out->base_enabled = selected == TC_BASE_POLICY_BAND_ONLY ? 0u : 1u;
    out->layout_capability = out->base_enabled != 0u
        ? (uint8_t)TC_BASE_LAYOUT_BASE_ONLY_PACKET
        : (uint8_t)TC_BASE_LAYOUT_BAND_ONLY;
    out->base_width = (uint16_t)base_w;
    out->base_height = (uint16_t)base_h;
    out->rollout_stage = (uint8_t)rollout_stage;
    out->fallback_reason = (uint8_t)fallback;
    return TC_OK;
}

int32_t tc_base_policy_choose(const topos_frame_config* config,
                              uint32_t max_dim, tc_base_policy requested,
                              uint32_t scalable_hint, uint32_t band_only_hint,
                              tc_base_policy_decision* out)
{
    return tc_base_policy_choose_gated(config, max_dim, requested,
                                       scalable_hint, band_only_hint,
                                       TC_BASE_ROLLOUT_EXPERIMENTAL, 0u, out);
}
