"""进程级 slice 线程数的协作式策略登记（复验 P1-17 / R-32）。

背景：native ABI（v2）帧级 API 无 context 对象，slice 线程数是进程级
全局（``tc_dev_set_thread_count``）。R6 的「ToposVideoEncoder open/abort
保存/恢复前一值」模式**不是线程安全的**——两个交错编码器 A/B 的
设置/恢复序列会互相覆盖，最终可能恢复到非原始值（复验最小场景：
A(8) open → B(1) open → B close 恢复 8 → A close 恢复 prev(=B open 前
的 8) ——看似巧合正确；但 A(8) → B(1) → A close 恢复 8（B 仍活性
能被改成 8）→ B close 恢复 1：期间 B 以 8 线程运行，违反声明）。

本模块以**登记表**取代嵌套保存/恢复：

- 编码器在首次编码前 ``register(key, n)``，close/abort 时 ``unregister``；
- 生效值 = 当前登记表里**最近登记**的偏好（后设先得语义保持——与
  R-32 文档一致），登记集变化时重算并一次性下发 native；
- 进程原始值（首个登记者之前）在表清空时恢复；
- per-context 线程数需 ABI 扩展，V2.1 评估（R-32）。

线程安全：进程级锁保护登记表；native 调用持锁串行化（set 为廉价原子写）。
"""
from __future__ import annotations

import logging
import threading
from typing import Dict, Optional

logger = logging.getLogger(__name__)


class SliceThreadsPolicy:
    """协作式 slice 线程数登记表（进程单例）。"""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._registry: Dict[str, int] = {}   # key → 偏好线程数
        self._roles: Dict[str, str] = {}      # key → 角色（P1-05 QoS 归因）
        self._order: list = []                # 登记顺序（最近在后）
        self._baseline: Optional[int] = None  # 首个登记者之前的进程值
        self._applied: Optional[int] = None   # 当前已下发 native 的值
        self._setter = None                   # 注入（测试/无 native 时）
        self._getter = None

    def bind(self, setter=None, getter=None) -> None:
        """绑定 native set/get（默认惰性接 topos_binding）。"""
        self._setter = setter
        self._getter = getter

    def _native_set(self, n: int) -> None:
        if self._setter is not None:
            self._setter(n)
            self._applied = n
            return
        try:
            from .topos_binding import ToposCodec

            codec = ToposCodec()
            got = codec.set_slice_threads(n)
            self._applied = int(got)
        except Exception:  # noqa: BLE001 — native 不可用时登记仍一致
            self._applied = None

    def _native_get(self) -> int:
        if self._getter is not None:
            return int(self._getter())
        try:
            from .topos_binding import ToposCodec

            return int(ToposCodec().slice_threads())
        except Exception:  # noqa: BLE001
            return 0

    def register(self, key: str, n: int, role: str = "encode") -> None:
        """登记偏好（首次编码前调用）；生效值 = 最近登记者。

        P1-05（过渡期）：role 记录登记方角色（'encode'/'export'/'playback'
        …）。native 只有一个进程级线程数值，导出登记接管时并发播放解码
        必然共享同一值——切换发生且存在异角色登记时记 QoS warning（可观测），
        per-context 预算待 V2.1 ABI。
        """
        with self._lock:
            if not self._registry and self._baseline is None:
                cur = self._native_get()
                if cur > 0:
                    self._baseline = cur
            switching_over = bool(self._registry) and \
                any(r != role for r in self._roles.values())
            if key in self._registry:
                self._order.remove(key)
            self._registry[key] = int(n)
            self._roles[key] = role
            self._order.append(key)
            self._apply_locked()
            if switching_over:
                others = sorted({r for k, r in self._roles.items() if k != key})
                if others:
                    logger.warning(
                        "slice 线程策略切换：%s(%d) 接管进程级值；并发 %s "
                        "侧同值受限（V2.1 per-context 前无独立预算）",
                        role, int(n), "/".join(others),
                    )

    def unregister(self, key: str) -> None:
        """注销（close/abort）；登记集变化时重算生效值。"""
        with self._lock:
            if key not in self._registry:
                return
            del self._registry[key]
            self._roles.pop(key, None)
            self._order.remove(key)
            self._apply_locked()

    def _apply_locked(self) -> None:
        if self._registry:
            want = self._registry[self._order[-1]]
        elif self._baseline is not None:
            want = self._baseline
        else:
            return
        if want != self._applied:
            self._native_set(want)

    def effective(self) -> Optional[int]:
        """当前生效偏好（诊断/测试用）。"""
        with self._lock:
            if self._registry:
                return self._registry[self._order[-1]]
            return self._baseline

    def reset_for_test(self) -> None:
        """测试钩子：清空登记/基线/已下发状态（不触碰 native 当前值，
        由调用方自行设置起始值）。"""
        with self._lock:
            self._registry.clear()
            self._roles.clear()
            self._order.clear()
            self._baseline = None
            self._applied = None

    def active_count(self) -> int:
        with self._lock:
            return len(self._registry)

    def describe(self) -> dict:
        """P1-05：登记表诊断（角色/生效值/QoS 归因用）。"""
        with self._lock:
            return {
                'effective': (
                    self._registry[self._order[-1]]
                    if self._registry else self._baseline
                ),
                'applied': self._applied,
                'baseline': self._baseline,
                'registrations': [
                    {'key': k, 'threads': self._registry[k],
                     'role': self._roles.get(k, 'encode')}
                    for k in self._order
                ],
            }


# 进程单例
SLICE_THREADS_POLICY = SliceThreadsPolicy()
