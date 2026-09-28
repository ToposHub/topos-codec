/* 库版本（独立于位流规范版本；位流版本见 bitstream_spec_v1.md §1） */
#ifndef TOPOS_CODEC_VERSION_H
#define TOPOS_CODEC_VERSION_H

#define TOPOS_CODEC_VERSION_MAJOR 0
#define TOPOS_CODEC_VERSION_MINOR 1
#define TOPOS_CODEC_VERSION_PATCH 0

/* 由 CMake 注入（-DTOPOS_GIT_COMMIT=...），缺省 unknown */
#ifndef TOPOS_GIT_COMMIT
#define TOPOS_GIT_COMMIT "unknown"
#endif
#ifndef TOPOS_BUILD_TARGET
#define TOPOS_BUILD_TARGET "unknown"
#endif

#endif /* TOPOS_CODEC_VERSION_H */
