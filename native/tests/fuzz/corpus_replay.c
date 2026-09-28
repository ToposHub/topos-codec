/* corpus 回放 driver：无 libFuzzer 环境下的确定性 fuzz 替身。
 *
 * 用法：
 *   topos_fuzz_replay <file>...      逐个文件回放（读取上限 1 MiB）
 *   topos_fuzz_replay -gen [N]       内置 LCG 生成 N（默认 4096）个确定性输入回放
 *
 * ctest 在所有 sanitizer 配置下运行 `-gen`（ADR-C003：fuzz 覆盖不依赖 libFuzzer）。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

#define REPLAY_MAX_BYTES (1024u * 1024u)

static int replay_path(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { return 0; } /* 缺失文件按跳过处理（corpus 可为空） */
    uint8_t* buf = (uint8_t*)malloc(REPLAY_MAX_BYTES);
    if (buf == NULL) { fclose(f); return 1; }
    size_t n = fread(buf, 1, REPLAY_MAX_BYTES, f);
    fclose(f);
    int r = LLVMFuzzerTestOneInput(buf, n);
    free(buf);
    return r != 0;
}

static int replay_generated(int count)
{
    uint64_t state = 0x243F6A8885A308D3ull;
    for (int i = 0; i < count; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const size_t len = (size_t)(state % 1024ull) + 16u;
        uint8_t* buf = (uint8_t*)malloc(len);
        if (buf == NULL) { return 1; }
        for (size_t j = 0; j < len; ++j) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            buf[j] = (uint8_t)(state >> 33);
        }
        const int r = LLVMFuzzerTestOneInput(buf, len);
        free(buf);
        if (r != 0) { return 1; }
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "-gen") == 0) {
        int n = 4096;
        if (argc >= 3) { n = atoi(argv[2]); }
        return replay_generated(n);
    }
    for (int i = 1; i < argc; ++i) {
        if (replay_path(argv[i]) != 0) {
            fprintf(stderr, "FUZZ INVARIANT VIOLATION: %s\n", argv[i]);
            return 1;
        }
    }
    return 0;
}
