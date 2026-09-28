/* 公共头 C++ 兼容（阶段 10）：extern "C" 包裹、布局 static_assert、
 * 可在 C++ 编译单元下调用全部查询类 API。encode/decode 路径与 C TU
 * 二进制一致（同一 C ABI），此处只验头文件本身可独立用于 C++ 集成方。 */
#include <cstddef>

#include "topos_codec.h"

static_assert(sizeof(topos_version_info) == 56, "topos_version_info ABI");
static_assert(sizeof(topos_cpu_features) == 32, "topos_cpu_features ABI");
static_assert(sizeof(topos_frame_config) == 64, "topos_frame_config ABI");
static_assert(sizeof(topos_frame_input) == 104, "topos_frame_input ABI");
static_assert(sizeof(topos_frame_stats) == 68, "topos_frame_stats ABI");
static_assert(sizeof(topos_frame_output) == 580, "topos_frame_output ABI");
static_assert(sizeof(topos_io) == 64, "topos_io ABI");
static_assert(sizeof(topos_movie_config) == 68, "topos_movie_config ABI");
static_assert(sizeof(topos_movie_info) == 80, "topos_movie_info ABI");
static_assert(offsetof(topos_frame_output, slice_status) == 36, "slice_status offset");

int main()
{
    if (tc_abi_version() != TOPOS_CODEC_ABI_VERSION) { return 1; }
    topos_version_info info = {};
    if (tc_version(&info) != TC_OK) { return 1; }
    topos_cpu_features cf = {};
    if (tc_query_cpu_features(&cf) != TC_OK) { return 1; }
    if (tc_query_support(3, 0, 10, 0) != TC_OK) { return 1; }
    topos_frame_config cfg = {}; /* 聚合零初始化（C++ 语义）必须可用 */
    cfg.visible_width = 64;
    cfg.visible_height = 48;
    if (tc_frame_config_validate(&cfg) != TC_OK) { return 1; }
    return 0;
}
