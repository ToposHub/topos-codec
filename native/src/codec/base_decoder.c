#include "base_decoder.h"

#include "../bitstream/packet.h"
#include "../common/error.h"
#include "../transform/plane.h"

int32_t tc_base_frame_decode(const uint8_t* data, size_t size, uint32_t max_dim,
                             uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                             const size_t strides[TC_FRAME_MAX_PLANES],
                             topos_frame_output* out_info)
{
    if (data == NULL || out_info == NULL || max_dim == 0u ||
        max_dim > TC_PLANE_MAX_DIM) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base decode arguments invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_packet_view view;
    int32_t rc = tc_packet_parse_structure(data, size, &view);
    if (rc != TC_OK) { return rc; }
    if (view.fh.visible_width > max_dim || view.fh.visible_height > max_dim) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "base packet geometry %ux%u exceeds max_dim %u",
                     (unsigned)view.fh.visible_width,
                     (unsigned)view.fh.visible_height, (unsigned)max_dim);
        return TC_ERR_INVALID_ARGUMENT;
    }
    return tc_frame_decode(data, size, planes_out, strides, out_info);
}
