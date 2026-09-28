"""阶段 10：并发与浸泡（Python 绑定层）。

- 多线程共享 ToposCodec + 各自 ToposMovieFile 随机 seek/读取/解码（播放/拖拽模拟）；
  断言 packet 字节一致、解码无 concealment —— C 库线程契约（帧级 API 纯函数）
  在绑定层的落实验证（C 侧契约测试见 test_robust_io）。
- 浸泡：重复解码无显著 RSS 增长（绑定层无泄漏 accumulate）。
- 能力协商 query_support。
"""
from __future__ import annotations

import ctypes
import gc
import os
import random
import subprocess
import sys
import threading

import pytest

from topos_codec import topos_binding
from topos_codec.topos_binding import (
    TC_OK,
    ToposCodec,
    ToposDecoder,
    ToposMovieFile,
    ToposMuxFile,
)

W, H, FRAMES = 320, 180, 24
CW = (W + 1) // 2


@pytest.fixture(scope="module")
def codec() -> ToposCodec:
    explicit = os.environ.get("TOPOS_CODEC_LIB")
    if explicit is None and not any(
        p.is_file() for p in topos_binding.candidate_library_paths()
    ):
        pytest.skip("libtopos_codec 未构建；先运行 bash native/run_tests.sh")
    return ToposCodec()


def _pack_u16(values: list[int]) -> bytes:
    import struct

    return struct.pack(f"<{len(values)}H", *values)


def _frame_planes(frame_i: int) -> tuple[bytes, bytes, bytes, bytes]:
    y = _pack_u16([(r * 4 + c + frame_i * 7) % 1024 for r in range(H) for c in range(W)])
    u = _pack_u16([c * 1023 // (CW - 1) for _ in range(H) for c in range(CW)])
    v = _pack_u16([r * 1023 // (H - 1) for r in range(H) for _ in range(CW)])
    a = _pack_u16([c * 65535 // (W - 1) for _ in range(H) for c in range(W)])
    return y, u, v, a


@pytest.fixture(scope="module")
def movie_path(codec: ToposCodec, tmp_path_factory) -> str:
    fc = topos_binding._CFrameConfig()
    fc.struct_size = ctypes.sizeof(topos_binding._CFrameConfig)
    fc.abi_version = codec.abi_version
    fc.visible_width, fc.visible_height = W, H
    fc.qp_base = 32
    fc.qmatrix_id = 1
    fc.alpha_mode = 2
    fc.alpha_bit_depth = 12
    fc.color_range = 1
    fc.color_primaries = 1
    fc.color_transfer = 1
    fc.color_matrix = 1
    fc.sar_num, fc.sar_den = 1, 1

    path = str(tmp_path_factory.mktemp("stage10") / "concurrent.mov")
    mux = ToposMuxFile(
        codec, path,
        codec.movie_config(width=W, height=H, qp=32, qmatrix=1,
                           alpha_mode=2, alpha_bit_depth=12),
    )
    for i in range(FRAMES):
        packet, stats = codec.encode_frame(fc, _frame_planes(i))
        assert stats.packet_size == len(packet)
        mux.add_packet(packet, i * 1000, 1000)
    mux.finish()
    mux.close()
    return path


def test_query_support(codec: ToposCodec) -> None:
    assert codec.query_support(3, 0, 10, 0) == TC_OK
    assert codec.query_support(3, 0, 10, 2) == TC_OK
    assert codec.query_support(4, 0, 10, 0) == topos_binding.TC_ERR_UNSUPPORTED_PROFILE
    # R4.1（v1.2 枚举扩展）：YUV 4:2:2 12-bit 已支持
    assert codec.query_support(3, 0, 12, 0) == TC_OK
    assert codec.query_support(3, 0, 11, 0) == topos_binding.TC_ERR_UNSUPPORTED_PIXEL_FORMAT
    # R4.2（v1.3 枚举扩展）：YUV 4:4:4（pf=1）10/12-bit 已支持
    assert codec.query_support(3, 1, 10, 0) == TC_OK
    assert codec.query_support(3, 1, 12, 0) == TC_OK
    # R4.3（v1.4 枚举扩展）：GBR 4:4:4（pf=2，matrix=0 identity）已支持
    assert codec.query_support(3, 2, 10, 0) == TC_OK
    assert codec.query_support(3, 2, 12, 0) == TC_OK
    assert codec.query_support(3, 3, 10, 0) == topos_binding.TC_ERR_UNSUPPORTED_PIXEL_FORMAT
    # R4.4：Pro444（profile 5）= 4:4:4 10/12；Extreme（profile 6）= 4:4:4 12 only
    assert codec.query_support(5, 1, 10, 0) == TC_OK
    assert codec.query_support(5, 2, 12, 0) == TC_OK
    assert codec.query_support(6, 2, 12, 0) == TC_OK
    assert codec.query_support(5, 0, 10, 0) == topos_binding.TC_ERR_UNSUPPORTED_PIXEL_FORMAT
    assert codec.query_support(6, 1, 10, 0) == topos_binding.TC_ERR_UNSUPPORTED_PIXEL_FORMAT
    assert codec.query_support(4, 0, 10, 0) == topos_binding.TC_ERR_UNSUPPORTED_PROFILE
    assert codec.query_support(3, 0, 10, 3) == topos_binding.TC_ERR_UNSUPPORTED_ALPHA_MODE


def test_concurrent_playback_and_scrub(codec: ToposCodec, movie_path: str) -> None:
    """4 线程 × 随机 seek：packet 与首帧一致（同内容帧）、解码无 concealment。"""
    movie0 = ToposMovieFile(codec, movie_path)
    ref_packets = [movie0.packet(i) for i in range(FRAMES)]
    movie0.close()

    errors: list[str] = []
    barrier = threading.Barrier(4)

    def worker(seed: int) -> None:
        rng = random.Random(seed)
        movie = ToposMovieFile(codec, movie_path)
        barrier.wait()
        try:
            for _ in range(60):
                idx = rng.randrange(FRAMES)
                pkt = movie.packet(idx)
                if pkt != ref_packets[idx]:
                    errors.append(f"t{seed}: packet {idx} 与参考不一致")
                    return
                frame = codec.decode(pkt)
                if frame.info.concealed_slices != 0:
                    errors.append(f"t{seed}: 帧 {idx} concealed")
                    return
                if frame.info.visible_width != W:
                    errors.append(f"t{seed}: 几何不符")
                    return
        finally:
            movie.close()

    threads = [threading.Thread(target=worker, args=(s,)) for s in (11, 22, 33, 44)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=120)
    assert not any(t.is_alive() for t in threads), "并发线程超时未退出"
    assert errors == []


def test_decode_soak_rss_converges(codec: ToposCodec, movie_path: str) -> None:
    """浸泡：有状态解码器 + 预分配 views 连续解码，RSS 增长必须近零（native 泄漏门）。

    macOS 的 ru_maxrss 单位是字节、Linux 是 KiB —— 统一归一到 KiB；
    采样为 gc 后的【当前 RSS】（ps），避开 maxrss 的历史峰值口径。

    复校（2026-09-12，C3 速度计划）：两处口径修正——
    1. 绑定层 _encode_scratch 稳态重用修复（旧版每帧重建 32MB 启发式
       缓冲）移除了 fixture 编码期对 maxrss 的预抬峰，旧两轮窗口判据
       在 Python 分配器阵发噪声（每 200 帧数~十余 MiB 的 arena 突刺）
       下不再可判；
    2. 无状态 decode() 每帧新分配输出平面，RSS 增长混杂 pymalloc 噪声
       ——改用产品热路径（ToposDecoder + make_views 预分配），增长
       即纯 native：实测每轮 ≤ 64 KiB（3 样本 × 6 轮 × 200 帧）。
    判据：深预热后 3×200 帧，总增长 ≤ 4 MiB（~60× 实测水位；任何
    逐帧 KB 级 native 泄漏必超）。
    """
    movie = ToposMovieFile(codec, movie_path)
    packets = [movie.packet(i) for i in range(FRAMES)]
    movie.close()

    def rss_kib() -> int:
        gc.collect()
        if sys.platform == "win32":
            # 当前工作集（字节）→ KiB；与 ps/VmRSS 同为"当前驻留"口径。
            # 仅 stdlib：PROCESS_MEMORY_COUNTERS via psapi/GetProcessMemoryInfo
            import ctypes
            from ctypes import wintypes

            class _Pmc(ctypes.Structure):
                _fields_ = [
                    ("cb", wintypes.DWORD),
                    ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t),
                ]

            pmc = _Pmc()
            pmc.cb = ctypes.sizeof(_Pmc)
            k32 = ctypes.windll.kernel32
            psapi = ctypes.windll.psapi
            # HANDLE 是 64 位：不声明 argtypes/restype 时 Python int 按 32 位
            # 传参，伪句柄 -1 被截断 → GetLastError 122
            k32.GetCurrentProcess.restype = wintypes.HANDLE
            psapi.GetProcessMemoryInfo.argtypes = [
                wintypes.HANDLE, ctypes.POINTER(_Pmc), wintypes.DWORD]
            psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
            if not psapi.GetProcessMemoryInfo(
                    k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb):
                raise RuntimeError("GetProcessMemoryInfo 失败")
            return int(pmc.WorkingSetSize / 1024)
        if sys.platform == "darwin":
            out = subprocess.run(
                ["ps", "-o", "rss=", "-p", str(os.getpid())],
                capture_output=True, text=True).stdout
            return int(out.strip())
        with open("/proc/self/status") as fh:
            for line in fh:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        raise RuntimeError("VmRSS 不可用")

    np = topos_binding._np
    if np is None:
        pytest.skip("numpy 不可用（views 浸泡需要 numpy 平面）")

    dec = ToposDecoder(codec)
    cinfo = dec.prepare(packets[0])
    plane_count = cinfo.plane_count
    dims = [(cinfo.visible_height, cinfo.visible_width)]
    if cinfo.pixel_format == 0:  # 4:2:2 → 色度半宽
        dims.append((cinfo.visible_height, (cinfo.visible_width + 1) // 2))
        dims.append((cinfo.visible_height, (cinfo.visible_width + 1) // 2))
    else:
        dims.append((cinfo.visible_height, cinfo.visible_width))
        dims.append((cinfo.visible_height, cinfo.visible_width))
    if plane_count > 3:
        dims.append((cinfo.visible_height, cinfo.visible_width))
    arrays = [np.zeros(d, dtype="<u2") for d in dims[:plane_count]]
    views = dec.make_views(arrays)
    dec.decode_views(packets[0], views, plane_count)  # 几何/池就绪

    for i in range(400):  # 深预热：解码池高水位稳定
        dec.decode_views(packets[i % FRAMES], views, plane_count)

    base = rss_kib()
    for r in range(3):
        for i in range(200):
            dec.decode_views(packets[(i + r * 7) % FRAMES], views, plane_count)
    growth = rss_kib() - base
    dec.close()
    assert growth <= 4 * 1024, (
        f"RSS 不收敛：3×200 帧增长 +{growth} KiB（疑似 native 泄漏）"
    )
