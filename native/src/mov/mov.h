/* MOV 容器层内部头（阶段 5）。公共 ABI 见 include/topos_codec.h。 */
#ifndef TOPOS_INTERNAL_MOV_H
#define TOPOS_INTERNAL_MOV_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"

/* tc_frame_config_validate：movie 配置字段级复用（src/codec） */
int32_t tc_frame_config_validate(const topos_frame_config* cfg);

#endif /* TOPOS_INTERNAL_MOV_H */
