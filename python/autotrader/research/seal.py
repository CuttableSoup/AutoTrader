"""The hold-out seal.

Everything after SEAL_DATE is reserved for one final run of a frozen specification
(docs/NEXT-STEPS.md option B). Research loaders truncate to it. Reading past it needs an
explicit unseal with a reason, and the unseal is written to the trial ledger so it can
never happen quietly.

The C++ backtester enforces the same date (BacktestConfig seal_date).
"""
from __future__ import annotations

from autotrader.research.ledger import Ledger

SEAL_DATE = "2025-09-15"   # last session available to development; hold-out starts the next session
OPEN_END = "9999-12-31"


class SealError(RuntimeError):
    pass


def data_end(unseal: bool = False, reason: str | None = None, hypothesis: str = "", ledger: Ledger | None = None) -> str:
    """Last date a research loader may read."""
    if not unseal:
        return SEAL_DATE
    if not reason or not reason.strip():
        raise SealError("unsealing the hold-out requires a written reason")
    (ledger or Ledger()).append(kind="unseal", hypothesis=hypothesis or "-", n_configs=0,
                                data_window=f"> {SEAL_DATE}", notes=reason.strip())
    return OPEN_END


def check_window(end: str, unseal: bool) -> None:
    if end > SEAL_DATE and not unseal:
        raise SealError(f"end {end} is inside the sealed hold-out (after {SEAL_DATE}); pass unseal with a reason")
