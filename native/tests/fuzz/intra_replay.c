/* Exercise real V7-R2 decoder paths using frozen packets and deterministic
 * mutations with repaired payload CRCs (so mutations reach entropy/IDCT).
 * V 代际收纳（2026-09-13）：原宿主 V3 退役，改为回放 V7-R2 intra 网格
 * golden（golden_codec_v7r2_intra.bin）；CRC 修补变异类保持不变。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bitstream/packet.h"
#include "common/crc32.h"
#include "common/endian.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv)
{
    if (argc != 2) { return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (f == NULL) { return 1; }
    uint8_t header[24];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "TPC1", 4) != 0 || tc_load_be32(header + 8) != 16u) {
        fclose(f); return 1;
    }
    for (unsigned record = 0; record < 16u; ++record) {
        uint8_t length[4];
        if (fread(length, 1, 4, f) != 4) { fclose(f); return 1; }
        uint32_t n = tc_load_be32(length);
        if (n == 0u || n > 1024u * 1024u) { fclose(f); return 1; }
        uint8_t* packet = malloc(n);
        if (packet == NULL || fread(packet, 1, n, f) != n) {
            free(packet); fclose(f); return 1;
        }
        if ((record & 1u) == 0u) {
            topos_packet_view view;
            if (tc_packet_scan(packet, n, &view) != TC_OK ||
                view.fh.version_major != 7u || view.fh.entropy_mode != 7u) {
                free(packet); fclose(f); return 1; }
            (void)LLVMFuzzerTestOneInput(packet, n);
            uint8_t* changed = malloc(n);
            if (changed == NULL) { free(packet); fclose(f); return 1; }
            for (unsigned si = 0; si < view.slice_count; ++si) {
                size_t offset = (size_t)(view.payloads[si] - packet);
                uint32_t bytes = view.slices[si].slice_payload_size;
                for (unsigned flip = 0; flip < 64u && bytes != 0u; ++flip) {
                    memcpy(changed, packet, n);
                    size_t pos = (size_t)(flip * 7919u) % bytes;
                    changed[offset + pos] ^= (uint8_t)(1u << (flip % 8u));
                    tc_store_be32(changed + offset - 4u, tc_crc32(changed + offset, bytes));
                    (void)LLVMFuzzerTestOneInput(changed, n);
                }
            }
            for (size_t cut = 0; cut < n; cut += n / 64u + 1u) {
                (void)LLVMFuzzerTestOneInput(packet, cut);
            }
            free(changed);
        }
        free(packet);
    }
    int trailing = fgetc(f);
    fclose(f);
    return trailing == EOF ? 0 : 1;
}
