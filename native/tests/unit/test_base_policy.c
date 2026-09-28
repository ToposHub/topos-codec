/* RD4-07：策略选择不依赖试解，且显式暴露当前布局能力。 */
#include "codec/base_policy.h"
#include "mini_test.h"

#include <string.h>

static void base_config(topos_frame_config* cfg, uint16_t width, uint16_t height)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = (uint32_t)sizeof(*cfg);
    cfg->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg->visible_width = width;
    cfg->visible_height = height;
    cfg->profile = 3u;
    cfg->bit_depth = 10u;
}

int main(void)
{
    topos_frame_config cfg;
    tc_base_policy_decision d;
    base_config(&cfg, 2048u, 1152u);
    MT_CHECK_EQ_I64(tc_base_policy_choose(&cfg, 2048u,
                                          TC_BASE_POLICY_SCALABLE_FAST,
                                          100u, 100u, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_BAND_ONLY);
    MT_CHECK_EQ_U64(d.base_enabled, 0u);
    MT_CHECK_EQ_U64(d.fallback_reason, TC_BASE_FALLBACK_NONE);

    base_config(&cfg, 4096u, 2160u);
    MT_CHECK_EQ_I64(tc_base_policy_choose(&cfg, 2048u,
                                          TC_BASE_POLICY_SCALABLE_FAST,
                                          100u, 100u, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_BAND_ONLY);
    MT_CHECK_EQ_U64(d.fallback_reason, TC_BASE_FALLBACK_NONE);

    base_config(&cfg, 6144u, 3456u);
    MT_CHECK_EQ_I64(tc_base_policy_choose(&cfg, 2048u,
                                          TC_BASE_POLICY_SCALABLE_FAST,
                                          100u, 100u, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_BAND_ONLY);
    MT_CHECK_EQ_U64(d.base_enabled, 0u);
    MT_CHECK_EQ_U64(d.fallback_reason, TC_BASE_FALLBACK_EXPERIMENTAL);
    MT_CHECK_EQ_U64(d.base_width, 2048u);
    MT_CHECK_EQ_U64(d.base_height, 1152u);

    MT_CHECK_EQ_I64(tc_base_policy_choose_gated(
        &cfg, 2048u, TC_BASE_POLICY_SCALABLE_FAST, 100u, 100u,
        TC_BASE_ROLLOUT_OPT_IN, 0u, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_BAND_ONLY);
    MT_CHECK_EQ_U64(d.fallback_reason, TC_BASE_FALLBACK_NO_PUBLIC_WRITER);

    MT_CHECK_EQ_I64(tc_base_policy_choose_gated(
        &cfg, 2048u, TC_BASE_POLICY_SCALABLE_FAST, 100u, 100u,
        TC_BASE_ROLLOUT_OPT_IN, TC_BASE_GATE_PUBLIC_V7B_WRITER, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_SCALABLE_FAST);
    MT_CHECK_EQ_U64(d.base_enabled, 1u);
    MT_CHECK_EQ_U64(d.layout_capability, TC_BASE_LAYOUT_BASE_ONLY_PACKET);

    MT_CHECK_EQ_I64(tc_base_policy_choose_gated(
        &cfg, 2048u, TC_BASE_POLICY_SCALABLE_FAST, 100u, 100u,
        TC_BASE_ROLLOUT_DEFAULT, TC_BASE_GATE_PUBLIC_V7B_WRITER, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_BAND_ONLY);
    MT_CHECK_EQ_U64(d.fallback_reason, TC_BASE_FALLBACK_RELEASE_GATE);

    MT_CHECK_EQ_I64(tc_base_policy_choose_gated(
        &cfg, 2048u, TC_BASE_POLICY_SCALABLE_FAST, 100u, 100u,
        TC_BASE_ROLLOUT_DEFAULT,
        TC_BASE_GATE_PUBLIC_V7B_WRITER | TC_BASE_GATE_SIZE_PASS |
            TC_BASE_GATE_SPEED_PASS, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_SCALABLE_FAST);

    MT_CHECK_EQ_I64(tc_base_policy_choose(&cfg, 2048u,
                                          TC_BASE_POLICY_SIZE_GUARDED,
                                          120u, 100u, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_BAND_ONLY);
    MT_CHECK_EQ_U64(d.fallback_reason, TC_BASE_FALLBACK_EXPERIMENTAL);
    MT_CHECK_EQ_I64(tc_base_policy_choose(&cfg, 2048u,
                                          TC_BASE_POLICY_SIZE_GUARDED,
                                          80u, 100u, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_BAND_ONLY);
    MT_CHECK_EQ_U64(d.fallback_reason, TC_BASE_FALLBACK_EXPERIMENTAL);
    MT_CHECK_EQ_I64(tc_base_policy_choose_gated(
        &cfg, 2048u, TC_BASE_POLICY_SIZE_GUARDED, 80u, 100u,
        TC_BASE_ROLLOUT_OPT_IN, TC_BASE_GATE_PUBLIC_V7B_WRITER, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_SIZE_GUARDED);
    MT_CHECK_EQ_U64(d.fallback_reason, TC_BASE_FALLBACK_NONE);
    MT_CHECK_EQ_I64(tc_base_policy_choose(&cfg, 2048u,
                                          TC_BASE_POLICY_BAND_ONLY,
                                          80u, 100u, &d), TC_OK);
    MT_CHECK_EQ_U64(d.selected_policy, TC_BASE_POLICY_BAND_ONLY);

    MT_CHECK_EQ_I64(tc_base_policy_choose(&cfg, 0u, TC_BASE_POLICY_BAND_ONLY,
                                          0u, 0u, &d), TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_base_policy_choose_gated(
        &cfg, 2048u, TC_BASE_POLICY_SCALABLE_FAST, 0u, 0u,
        TC_BASE_ROLLOUT_OPT_IN, 0x80000000u, &d), TC_ERR_INVALID_ARGUMENT);
    MT_MAIN_RETURN();
}
