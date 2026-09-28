/* CLI 共享：topos_io 的 stdio 适配（阶段 5 三个 CLI 使用）。
 *
 * 大文件：_FILE_OFFSET_BITS 64（Linux）；macOS off_t 本就 64 位。
 * 读端额外做 length 探测（fseeko END + ftello）。 */
#ifndef TOPOS_CLI_FILE_H
#define TOPOS_CLI_FILE_H

#if !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TC_CLI_UNUSED __attribute__((unused))
#else
#define TC_CLI_UNUSED
#endif

/* 64 位 stdio 定位：POSIX 用 fseeko/ftello（off_t）；MSVC 无这些接口，
 * 直接用 _fseeki64/_ftelli64（天生 64 位，无 _FILE_OFFSET_BITS 概念） */
#if defined(_MSC_VER)
#define TC_FSEEK_OFF(f, o, whence) _fseeki64((f), (long long)(o), (whence))
#define TC_FTELL_OFF(f) _ftelli64((f))
#else
#define TC_FSEEK_OFF(f, o, whence) fseeko((f), (off_t)(o), (whence))
#define TC_FTELL_OFF(f) ftello((f))
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "topos_codec.h"

static TC_CLI_UNUSED int32_t tc_file_read(void* ctx, uint64_t off, void* buf, size_t n)
{
    FILE* f = (FILE*)ctx;
    if (n == 0u) { return TC_OK; }
    if (TC_FSEEK_OFF(f, off, SEEK_SET) != 0) { return TC_ERR_IO; }
    return fread(buf, 1u, n, f) == n ? TC_OK : TC_ERR_IO;
}

static TC_CLI_UNUSED int32_t tc_file_write(void* ctx, const void* d, size_t n)
{
    FILE* f = (FILE*)ctx;
    if (n == 0u) { return TC_OK; }
    return fwrite(d, 1u, n, f) == n ? TC_OK : TC_ERR_IO;
}

static TC_CLI_UNUSED int32_t tc_file_seek_write(void* ctx, uint64_t off, const void* d, size_t n)
{
    FILE* f = (FILE*)ctx;
    if (n == 0u) { return TC_OK; }
    if (TC_FSEEK_OFF(f, off, SEEK_SET) != 0) { return TC_ERR_IO; }
    return fwrite(d, 1u, n, f) == n ? TC_OK : TC_ERR_IO;
}

static TC_CLI_UNUSED void tc_io_sink_file(FILE* f, topos_io* io)
{
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(topos_io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = f;
    io->write = tc_file_write;
    io->seek_write = tc_file_seek_write;
}

static TC_CLI_UNUSED int tc_io_src_file(FILE* f, topos_io* io)
{
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(topos_io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = f;
    io->read = tc_file_read;
    if (TC_FSEEK_OFF(f, 0, SEEK_END) != 0) { return -1; }
    long long end = (long long)TC_FTELL_OFF(f);
    if (end < 0) { return -1; }
    io->length = (uint64_t)end;
    rewind(f);
    return 0;
}

#endif /* TOPOS_CLI_FILE_H */
