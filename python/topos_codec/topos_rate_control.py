"""Topos 档位码率的帧级控制器（对标达芬奇路线 阶段 1 → P4 帧级码控）。

语义：档位以目标码率定标，逐帧单遍固定 qp 出流 + 帧字节数反馈更新下一帧
qp——长期平均码率贴档位目标（VBV/CBR 式），单帧字节可浮动。

P4 帧级码控（2026-09-21，画质对齐计划 §5 P4）：防复杂帧被平均码率饿死。

- **短窗 VBV**：反馈 = 单帧比例项 + 短窗（默认 24 帧 ≈1s）偏差项 + 长程
  积分项——窗口项使 qp 对局部复杂度变化平滑跟踪，替代单纯上一帧反馈；
- **场景切换检测**：帧字节 > ``cut_factor`` × 窗口中位数 → 切换帧。切换
  帧按"借贷"处理（不引发下一帧 qp 尖峰），并把 :attr:`reseed_hint`
  置位——编码器据此对下一帧做 sized 搜索即刻重锚（配合编码器的帧内
  复杂度前瞻，复杂度突变的首帧即可拿到正确 qp）；
- **复杂帧借贷与封顶**：超支进 ``_debt``，在 ``repay_window`` 帧内按对数
  比例温和偿还（每帧额外 ≤ ``lend_repay_clamp`` qp）；单帧借贷上限
  ``lend_cap_factor`` × target（超限部分视为码控失配，qp 立即强修正）；
- **预算模式**（P1）：逐帧目标 = 剩余预算均摊 ×0.97 安全系数，
  :attr:`over_budget` 供 close() 硬裁决；
- **遥测**：逐帧 (index, qp, bytes, cut) 环形记录，:meth:`telemetry`
  输出摘要（P4 验收：QP/实际字节可观测）。

确定性（spec §11.1 序列级）：同输入帧序列 → 同 qp 序列 → 同输出字节；
qp 更新是帧字节数历史的纯函数（中位数/求和/对数全确定性路径）。
"""

from __future__ import annotations

import math
from collections import deque
from typing import List, Optional, Tuple


class ToposRateFeedback:
    """单遍码控反馈状态（每个编码器实例一份）。

    模型：bytes ≈ C·2^(−qp/8)（帧内搜索的粗步长 8 几何），故目标比例 r 的
    qp 修正为 Δqp = 8·log2(r)。比例项每帧钳位 ±4（切换帧下一帧放宽 ±8，
    配合重锚）；叠加短窗/长程偏差的弱积分消除稳态残差。

    用法：首帧由 sized 搜索定标后 :meth:`seed`；此后每单遍帧 :meth:`note`；
    复杂度重锚（编码器前瞻/切换检测）用 :meth:`anchored`（只更新 qp，
    不清预算累计与窗口历史）。
    """

    #: 预算模式逐帧目标安全系数：反馈控制存在单帧滞后与 qp 量化，贴线
    #: 均摊会以 ~0.02% 量级累积溢出（2K proxy 实测 +15KB/88MB 即触顶拒
    #: 收）——预留 3% 收敛余量，长期利用率 ≥97% 仍属"用足预算"。
    BUDGET_TARGET_MARGIN = 0.97

    def __init__(self, target_bytes: int, qp_min: int = 0, qp_max: int = 95,
                 total_budget_bytes: Optional[int] = None,
                 total_frames: Optional[int] = None,
                 window: int = 24,
                 cut_factor: float = 1.8,
                 lend_cap_factor: float = 3.0,
                 repay_window: int = 12,
                 lend_repay_clamp: int = 2,
                 telemetry_cap: int = 4096) -> None:
        if target_bytes <= 0:
            raise ValueError(f"target_bytes 必须为正，得到 {target_bytes}")
        self.target = float(target_bytes)
        self.qp_min = int(qp_min)
        self.qp_max = int(qp_max)
        self.qp: Optional[int] = None
        self.total = 0
        self.frames = 0
        if (total_budget_bytes is None) != (total_frames is None):
            raise ValueError(
                "预算模式须同时给出 total_budget_bytes 与 total_frames"
                f"（得到 {total_budget_bytes!r}/{total_frames!r}）")
        if total_budget_bytes is not None:
            if int(total_budget_bytes) <= 0 or int(total_frames) <= 0:
                raise ValueError(
                    f"预算参数必须为正（{total_budget_bytes}/{total_frames}）")
        self.budget = int(total_budget_bytes) if total_budget_bytes is not None \
            else None
        self.total_frames = int(total_frames) if total_frames is not None else None
        # P4 短窗/切换/借贷参数
        self.window = max(2, int(window))
        self.cut_factor = float(cut_factor)
        self.lend_cap_factor = float(lend_cap_factor)
        self.repay_window = max(1, int(repay_window))
        self.lend_repay_clamp = int(lend_repay_clamp)
        self._win: deque = deque(maxlen=self.window)
        self._debt = 0
        self._telemetry: List[Tuple[int, int, int, int]] = []
        self._telemetry_cap = int(telemetry_cap)
        self.last_frame_was_cut = False
        self.reseed_hint = False

    @property
    def active(self) -> bool:
        """首帧定标完成（可单遍出流）。"""
        return self.qp is not None

    @property
    def over_budget(self) -> bool:
        """预算模式：累计视频字节已超硬上限（close 据此拒绝定稿）。"""
        return self.budget is not None and self.total > self.budget

    # —— 内部 ——

    def _refresh_target(self) -> None:
        """预算模式：逐帧目标 = 剩余预算 / 剩余帧（×安全系数）。"""
        if self.budget is None or self.total_frames is None:
            return
        remaining_frames = max(1, self.total_frames - self.frames)
        remaining_bytes = self.budget - self.total
        safe = int(remaining_bytes * self.BUDGET_TARGET_MARGIN)
        self.target = float(max(1, safe // remaining_frames))

    def _record(self, size: int) -> None:
        if len(self._telemetry) < self._telemetry_cap:
            self._telemetry.append(
                (self.frames, int(self.qp or 0), int(size),
                 1 if self.last_frame_was_cut else 0))

    def _win_median(self) -> float:
        """窗口字节中位数（确定性：排序小数组）。"""
        if not self._win:
            return float(self.target)
        s = sorted(self._win)
        n = len(s)
        mid = n // 2
        return float(s[mid]) if n % 2 == 1 else 0.5 * (s[mid - 1] + s[mid])

    def _account(self, packet_size: int) -> None:
        """公共记账：窗口/累计/切换检测/借贷/遥测（seed 与 note 共用）。"""
        size = int(packet_size)
        prev_median = self._win_median()
        self._win.append(size)
        self.total += size
        self.frames += 1
        is_cut = (len(self._win) >= 5
                  and size > self.cut_factor * max(1.0, prev_median))
        self.last_frame_was_cut = is_cut
        if is_cut:
            self.reseed_hint = True
        # 借贷：切换帧/单帧超支进债务，封顶 lend_cap_factor × target
        overshoot = size - self.target
        if overshoot > 0:
            capped = min(overshoot, self.lend_cap_factor * self.target)
            self._debt += int(capped)
            if overshoot > self.lend_cap_factor * self.target:
                # 超封顶 = 码控失配（qp 远偏离目标），置强修正信号
                self.reseed_hint = True
        elif self._debt > 0:
            self._debt = max(0, self._debt + int(overshoot))
        self._record(size)
        self._refresh_target()

    def _next_qp(self, size: int) -> int:
        """控制律：比例 + 短窗 VBV + 长程积分 + 债务偿还 → 下一帧 qp。"""
        t = max(1.0, self.target)
        prop = 8.0 * math.log2(max(int(size), 1) / t)
        # 短窗 VBV：窗口均值 vs 当前目标（窗口满帧后生效）
        win_term = 0.0
        if len(self._win) >= max(4, self.window // 2):
            win_term = 8.0 * math.log2(
                (sum(self._win) / len(self._win)) / t)
            win_term = max(-6.0, min(6.0, win_term))
        integ = 0.0
        if self.frames > 4:
            integ = 8.0 * math.log2(self.total / (t * self.frames))
        # 债务偿还：欠债量折算为窗口均摊比例的对数修正（温和，封顶）
        repay = 0.0
        if self._debt > 0:
            repay = 8.0 * math.log2(
                1.0 + self._debt / (t * self.repay_window))
            repay = min(float(self.lend_repay_clamp), repay)
        adj = prop + 0.35 * win_term + 0.15 * integ + repay
        # 常规钳位 ±4；切换帧后的首帧放宽 ±8（快速重锚窗口）
        clamp = 8 if self.last_frame_was_cut else 4
        adj = max(-clamp, min(clamp, adj))
        qp = int(self.qp or 0) + int(round(adj))
        return max(self.qp_min, min(self.qp_max, qp))

    # —— 对外 ——

    def seed(self, qp_used: int, packet_size: int) -> None:
        """首帧 sized 搜索定标（q* 唯一，确定性起点）。"""
        self.qp = min(self.qp_max, max(self.qp_min, int(qp_used)))
        self._account(packet_size)

    def anchored(self, qp_used: int, packet_size: int) -> None:
        """复杂度重锚（编码器前瞻/切换检测触发的 sized 搜索结果）：
        只更新 qp，不清预算累计/窗口/债务（与首帧 seed 的区别）。"""
        self.qp = min(self.qp_max, max(self.qp_min, int(qp_used)))
        self._account(packet_size)
        self.reseed_hint = False

    def note(self, packet_size: int) -> int:
        """记录一帧单遍编码结果，返回下一帧应使用的 qp。

        切换检测在 :meth:`_account` 内完成（本帧字节 vs 窗口中位数）；
        置位 :attr:`reseed_hint` 供编码器对下一帧做 sized 重锚，置位
        ``last_frame_was_cut`` 放宽下一帧的 qp 调整钳位。
        """
        if self.qp is None:
            raise RuntimeError("ToposRateFeedback 未定标（先 seed）")
        size = int(packet_size)
        self._account(size)
        self.qp = self._next_qp(size)
        return self.qp

    def stats(self) -> Tuple[int, int, float]:
        """(已编码帧数, 累计字节, 平均码率偏差)。"""
        avg_dev = (self.total / (self.target * self.frames) - 1.0) if self.frames else 0.0
        return self.frames, self.total, avg_dev

    def telemetry(self) -> dict:
        """P4 遥测：逐帧 (index, qp, bytes, cut) + 摘要。"""
        cuts = [t for t in self._telemetry if t[3]]
        qps = [t[1] for t in self._telemetry]
        sizes = [t[2] for t in self._telemetry]
        return {
            "frames_recorded": len(self._telemetry),
            "qp_min": min(qps) if qps else None,
            "qp_max": max(qps) if qps else None,
            "qp_last": qps[-1] if qps else None,
            "bytes_total": sum(sizes),
            "bytes_max_frame": max(sizes) if sizes else None,
            "cut_frames": [t[0] for t in cuts],
            "debt_bytes": self._debt,
            "reseed_hint": self.reseed_hint,
            "per_frame": list(self._telemetry),
        }
