/* half_map —— HALF 样本域公共 ABI 承载（tc_image_half_to_codes / codes_to_half）。
 *
 * 映射定义与冻结依据见 half_map.h / spec §15；本 TU 只做参数校验与批量循环
 * （标量规范在头文件 inline，批量与单元素逐位一致由单测钉死）。
 */
#include "image/half_map.h"

#include "common/error.h"
#include "topos_codec.h"

int32_t tc_image_half_to_codes(const uint16_t* half, size_t n, uint16_t* codes)
{
    if (half == NULL || codes == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "half_to_codes: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    tci_half_map_plane(half, n, codes);
    return TC_OK;
}

int32_t tc_image_codes_to_half(const uint16_t* codes, size_t n, uint16_t* half)
{
    if (codes == NULL || half == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "codes_to_half: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    tci_code_unmap_plane(codes, n, half);
    return TC_OK;
}

void tci_half_map_plane(const uint16_t* src, size_t n, uint16_t* dst)
{
    for (size_t i = 0; i < n; ++i) {
        dst[i] = tci_half_to_code(src[i]);
    }
}

void tci_code_unmap_plane(const uint16_t* src, size_t n, uint16_t* dst)
{
    for (size_t i = 0; i < n; ++i) {
        dst[i] = tci_code_to_half(src[i]);
    }
}
