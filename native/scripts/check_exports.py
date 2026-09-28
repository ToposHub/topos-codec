#!/usr/bin/env python3
"""check_exports.py — 跨平台公共符号表钉死（R7）。

用 stdlib 解析 PE / ELF / Mach-O 导出表，与
``tests/abi/public_symbols_v1.txt`` 精确匹配（缺失即失败）。这是
``run_tests.sh`` 符号检查的跨平台实现——CI Windows 无 ``nm``，macOS
``nm -gU`` 语义平台相关；本脚本三种平台走同一解析路径。

用法：
    python check_exports.py <lib-path> [symbols-file]

退出码：0 = 清单内符号全部在导出表中；1 = 有缺失 / 文件不可解析。
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_SYMBOLS = REPO / "tests" / "abi" / "public_symbols_v1.txt"


# ---------------------------------------------------------------- Mach-O
def _macho_exports(data: bytes) -> set:
    if len(data) < 32:
        raise ValueError("Mach-O 太短")
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == 0xFEEDFACF:
        endian, is64 = "<", True
    elif magic == 0xCFFAEDFE:
        endian, is64 = ">", True
    elif magic == 0xFEEDFACE:
        endian, is64 = "<", False
    elif magic == 0xCEFAEDFE:
        endian, is64 = ">", False
    else:
        raise ValueError(f"非 Mach-O magic: {magic:#x}")
    ncmds = struct.unpack_from(endian + "I", data, 16)[0]
    off = 32 if is64 else 28
    names = set()
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from(endian + "II", data, off)
        if cmd == 0x2:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from(
                endian + "iiii", data, off + 8)
            strtab = data[stroff:stroff + strsize]
            for i in range(nsyms):
                n_off = symoff + i * (16 if is64 else 12)
                strx = struct.unpack_from(endian + "I", data, n_off)[0]
                n_type = data[n_off + 4]
                # N_EXT=0x01 且非 UNDF（type&0x0e==0）→ 已定义导出
                if not (n_type & 0x01) or (n_type & 0x0E) == 0:
                    continue
                end = strtab.find(b"\x00", strx)
                name = strtab[strx:end].decode("ascii", "replace")
                # Mach-O 符号带前导下划线（C 调用约定），剥掉后与清单对齐
                if name.startswith("_"):
                    name = name[1:]
                if name:
                    names.add(name)
        off += cmdsize
    return names


# ---------------------------------------------------------------- ELF
def _elf_exports(data: bytes) -> set:
    if data[:4] != b"\x7fELF":
        raise ValueError("非 ELF magic")
    is64 = data[4] == 2
    endian = "<" if data[5] == 1 else ">"
    if is64:
        e_shoff = struct.unpack_from(endian + "Q", data, 0x28)[0]
        e_shentsize, e_shnum = struct.unpack_from(endian + "HH", data, 0x3A)
        sh_fmt = endian + "IIQQQQIIQQ"
    else:
        e_shoff = struct.unpack_from(endian + "I", data, 0x20)[0]
        e_shentsize, e_shnum = struct.unpack_from(endian + "HH", data, 0x2E)
        sh_fmt = endian + "IIIIIIIIII"
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sections.append(struct.unpack_from(sh_fmt, data, off))
    names = set()
    for sh in sections:
        sh_type = sh[1]
        if sh_type != 11:  # SHT_DYNSYM
            continue
        offset, sh_size, link = sh[4], sh[5], sh[6]
        ent = 24 if is64 else 16
        strtab = sections[link]
        str_off, str_size = strtab[4], strtab[5]
        strings = data[str_off:str_off + str_size]
        count = sh_size // ent
        for i in range(count):
            sym = data[offset + i * ent: offset + (i + 1) * ent]
            if is64:
                st_name, st_info = struct.unpack_from(endian + "IB", sym, 0)
                st_shndx = struct.unpack_from(endian + "H", sym, 6)[0]
            else:
                st_name, st_info = struct.unpack_from(endian + "IB", sym, 0)
                st_shndx = struct.unpack_from(endian + "H", sym, 14)[0]
            bind = st_info >> 4
            if st_shndx == 0 or bind not in (1, 2):  # SHN_UNDEF / GLOBAL|WEAK
                continue
            end = strings.find(b"\x00", st_name)
            name = strings[st_name:end].decode("ascii", "replace")
            if name:
                names.add(name)
    return names


# ---------------------------------------------------------------- PE
def _pe_exports(data: bytes) -> set:
    if data[:2] != b"MZ":
        raise ValueError("非 PE magic")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("PE 签名缺失")
    coff = e_lfanew + 4
    machine, nsec = struct.unpack_from("<HH", data, coff)
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    pe32plus = magic == 0x20B
    dd_off = opt + (112 if pe32plus else 96)
    export_rva, export_size = struct.unpack_from("<II", data, dd_off)

    # section 表：RVA → 文件偏移
    sec_off = opt + opt_size
    sections = []
    for i in range(nsec):
        s = sec_off + i * 40
        vsize, vaddr, rawsize, rawoff = struct.unpack_from("<IIII", data, s + 8)
        sections.append((vaddr, max(vsize, rawsize), rawoff))

    def rva2off(rva: int) -> int:
        for vaddr, span, rawoff in sections:
            if vaddr <= rva < vaddr + span:
                return rawoff + (rva - vaddr)
        raise ValueError(f"RVA {rva:#x} 不在任何 section")

    if export_rva == 0:
        return set()
    exp = rva2off(export_rva)
    # IMAGE_EXPORT_DIRECTORY（winnt.h）：
    #   0 Characteristics, 4 TimeDateStamp, 8/10 Major/MinorVersion,
    #   12 NameRVA, 16 Base, 20 NumberOfFunctions, 24 NumberOfNames,
    #   28 AddressOfFunctions, 32 AddressOfNames, 36 AddressOfNameOrdinals
    (_chars, _stamp, _maj, _min, _name_rva, _base, _n_funcs,
     n_names, _addr_funcs, addr_names_rva) = struct.unpack_from(
        "<IIHHIIIIII", data, exp)
    names = set()
    for i in range(n_names):
        rva = struct.unpack_from("<I", data, rva2off(addr_names_rva) + i * 4)[0]
        off = rva2off(rva)
        end = data.find(b"\x00", off)
        name = data[off:end].decode("ascii", "replace")
        if name:
            names.add(name)
    return names


def read_exports(path: Path) -> set:
    data = path.read_bytes()
    magic = struct.unpack_from("<I", data[:4])[0]
    if data[:2] == b"MZ":
        return _pe_exports(data)
    if data[:4] == b"\x7fELF":
        return _elf_exports(data)
    if magic in (0xFEEDFACF, 0xFEEDFACE, 0xCFFAEDFE, 0xCEFAEDFE):
        return _macho_exports(data)
    # fat/universal 二进制：按 fat_arch 表取第一个 slice 的真实 offset
    # （对齐填充使 slice 常不紧跟表尾——R8 复审 C-2 实证 offset=16384）
    if magic in (0xCAFEBABE, 0xBEBAFECA):
        nfat = struct.unpack_from(">I", data, 4)[0]
        if 0 < nfat <= 8:
            # fat_arch（big-endian）：cputype, cpusubtype, offset, size, align
            _, _, slice_off, _, _ = struct.unpack_from(">iiIII", data, 8)
            return read_exports_at(data, slice_off)
    raise ValueError(f"无法识别的库格式: {path}")


def read_exports_at(data: bytes, offset: int) -> set:
    sub = data[offset:]
    magic = struct.unpack_from("<I", sub[:4])[0]
    if sub[:4] == b"\x7fELF":
        return _elf_exports(sub)
    if magic in (0xFEEDFACF, 0xFEEDFACE, 0xCFFAEDFE, 0xCEFAEDFE):
        return _macho_exports(sub)
    if sub[:2] == b"MZ":
        return _pe_exports(sub)
    raise ValueError(f"fat slice 不支持: {magic:#x}")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    lib = Path(sys.argv[1])
    symfile = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_SYMBOLS
    if not lib.is_file():
        print(f"库不存在: {lib}")
        return 1
    required = [ln.strip() for ln in symfile.read_text().splitlines()
                if ln.strip() and not ln.startswith("#")]
    try:
        exports = read_exports(lib)
    except (ValueError, struct.error) as exc:
        print(f"解析失败 {lib}: {exc}")
        return 1
    missing = [s for s in required if s not in exports]
    if missing:
        for s in missing:
            print(f"缺失公共符号: {s}")
        print(f"符号表钉死失败（导出 {len(exports)} 个，清单 {len(required)} 个）")
        return 1
    print(f"公共符号表: OK（导出 {len(exports)} 个 ≥ 清单 {len(required)} 个）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
