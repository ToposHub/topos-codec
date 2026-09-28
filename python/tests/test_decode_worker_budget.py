"""D3 cooperative decoder-context worker budget tests."""
from __future__ import annotations

from topos_codec.decode_worker_budget import DecodeWorkerBudget


def test_budget_rebalances_and_restores() -> None:
    budget = DecodeWorkerBudget(capacity=8)
    applied: dict[str, int] = {}

    a = budget.acquire("a", role="playback", apply=lambda n: applied.__setitem__("a", n))
    assert a.assigned == 8
    assert applied["a"] == 8

    b = budget.acquire("b", role="export", apply=lambda n: applied.__setitem__("b", n))
    assert a.assigned == 4
    assert b.assigned == 4
    assert applied["a"] == applied["b"] == 4
    assert budget.describe()["assigned_total"] == 8

    b.release()
    assert a.assigned == 8
    assert applied["a"] == 8
    a.release()
    assert budget.describe()["active"] == 0


def test_budget_honors_caps_and_reports_oversubscription() -> None:
    budget = DecodeWorkerBudget(capacity=2)
    a = budget.acquire("a", requested=1)
    b = budget.acquire("b", requested=1)
    c = budget.acquire("c", requested=1)
    state = budget.describe()
    assert [entry["assigned"] for entry in state["leases"]] == [1, 1, 1]
    assert state["oversubscribed"] is True
    c.release()
    b.release()
    a.release()

