"""Brute-force sweep plumbing: arm wiring, grid sizing, DSR/PBO glue. No vendor data,
no C++ binary required (DSR-CLI tests skip themselves when at_dsr is not built)."""
from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from autotrader.research import sweep
from autotrader.research.panel import Panel


def synth_panel(T: int = 1400, N: int = 30, seed: int = 4) -> Panel:
    rng = np.random.default_rng(seed)
    dates = pd.bdate_range("2017-01-02", periods=T).to_numpy().astype("datetime64[D]")
    rets = rng.normal(0.0002, 0.01, (T, N))
    close = 50.0 * np.cumprod(1 + rets, axis=0)
    px = {"open": close.copy(), "close": close.copy(), "closeadj": close.copy(), "closeunadj": close.copy(),
          "volume": np.full((T, N), 1e6)}
    fund = pd.DataFrame([{"ticker": f"S{j}", "dimension": d, "date": q, "sharesbas": 2e8, "sharefactor": 1.0,
                          "assets": 1e9, "equity": 4e8, "debt": 3e8, "netinc": 5e7, "ncfo": 6e7, "gp": 2e8}
                         for j in range(N) for q in pd.date_range("2016-06-01", periods=32, freq="QS") for d in ("ARQ", "ART")])
    return Panel(dates=dates, symbols=np.array([f"S{j}" for j in range(N)]), px=px,
                 spy_open_tr=np.full(T, 100.0), spy_close_tr=np.full(T, 100.0),
                 first_price=np.full(N, np.datetime64("2010-01-01")), sector=np.array(["X"] * N),
                 fundamentals=fund, end="2099-01-01")


def test_n_configs_matches_the_declared_grid_size():
    assert sweep.N_CONFIGS == (len(sweep.RAW_SIGNAL_NAMES) + len(sweep.COMPOSITE_FAMILIES)) * \
        len(sweep.N_BUCKETS_GRID) * len(sweep.HORIZONS) * len(sweep.BANDS)


def test_build_arms_covers_every_declared_signal_and_family(tmp_path):
    p = synth_panel(T=900, N=20)
    arms = sweep.build_arms(p)
    assert set(arms) == set(sweep.RAW_SIGNAL_NAMES) | set(sweep.COMPOSITE_FAMILIES)
    f = int(p.month_end_indices()[-1])
    elig = p.universe_mask()[f]
    for name, fn in arms.items():
        out = fn(f, elig)
        assert out.shape == (p.N,), name    # every arm returns one score per symbol, no crashes


def test_run_grid_on_a_trimmed_grid(monkeypatch):
    monkeypatch.setattr(sweep, "N_BUCKETS_GRID", (5,))
    monkeypatch.setattr(sweep, "HORIZONS", (20,))
    monkeypatch.setattr(sweep, "BANDS", ("all",))
    p = synth_panel(T=1400, N=25)
    cells = sweep.run_grid(p, first_formation="2020-09-01")
    n_arms = len(sweep.RAW_SIGNAL_NAMES) + len(sweep.COMPOSITE_FAMILIES)
    assert len(cells) == n_arms                   # 1 bucket-count x 1 horizon x 1 band each
    assert all(c.horizon == 20 and c.band == "all" and c.n_buckets == 5 for c in cells)
    assert all(c.returns.shape[0] == len(p.month_end_indices(start="2020-09-01")) for c in cells)


def test_run_pbo_on_synthetic_cells():
    rng = np.random.default_rng(2)
    F = 60
    cells = [sweep.Cell("noise", 5, 20, "all", {}, rng.normal(0, 0.02, F)) for _ in range(20)]
    cells.append(sweep.Cell("edge", 5, 20, "all", {}, rng.normal(0.01, 0.02, F)))   # a real, consistent edge
    res = sweep.run_pbo(cells, n_splits=8)
    assert res["n_configs"] == 21
    assert 0.0 <= res["pbo"] <= 1.0


def test_run_pbo_requires_at_least_two_usable_cells():
    cells = [sweep.Cell("thin", 5, 20, "all", {}, np.full(60, np.nan))]
    with pytest.raises(RuntimeError):
        sweep.run_pbo(cells)


def test_find_dsr_cli_explicit_missing_path_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        sweep.find_dsr_cli(tmp_path / "does_not_exist")


def test_find_dsr_cli_explicit_existing_path(tmp_path):
    exe = tmp_path / "at_dsr"
    exe.write_text("")
    assert sweep.find_dsr_cli(exe) == exe


@pytest.fixture
def dsr_cli_path():
    try:
        return sweep.find_dsr_cli()
    except FileNotFoundError:
        pytest.skip("at_dsr not built; see CLAUDE.md for the cygwin build command")


def test_run_dsr_against_the_real_cli(dsr_cli_path):
    rng = np.random.default_rng(5)
    F = 60
    cells = [sweep.Cell("noise", 5, 20, "all", {}, rng.normal(0, 0.02, F)) for _ in range(30)]
    cells.append(sweep.Cell("edge", 5, 20, "all", {}, rng.normal(0.01, 0.02, F)))
    result = sweep.run_dsr(cells, periods_per_year=12.0, dsr_cli=dsr_cli_path)
    assert result["n_trials"] == 31
    assert result["best_cell"]["arm"] == "edge"
    assert "deflated_sharpe_annual" in result and "var_sr_trials" in result


def test_render_report_and_write_outputs(tmp_path):
    ok = np.full(20, 0.01)
    thin = np.full(20, np.nan)
    thin[:3] = 0.0                     # fewer than MIN_FORMATIONS valid points -> excluded from ranking
    cells = [sweep.Cell("a", 5, 60, "all", {"top_mean_pct": 1.0, "top_nw_t": 2.0, "spearman": 0.9, "horizon": 60}, ok),
            sweep.Cell("b", 5, 60, "all", {"top_mean_pct": -1.0, "top_nw_t": -0.5, "spearman": 0.1, "horizon": 60}, ok),
            sweep.Cell("thin", 5, 60, "all", {"top_mean_pct": 0.0, "top_nw_t": 0.0, "spearman": float("nan"), "horizon": 60}, thin)]
    dsr = {"deflated_sharpe_annual": 0.3, "n_trials": 3, "dsr_probability": 0.6, "best_cell": {"arm": "a", "n_buckets": 5, "horizon": 60, "band": "all"}}
    pbo_result = {"pbo": 0.2, "n_splits_evaluated": 10, "n_configs": 3}
    md = sweep.render_report(cells, dsr, pbo_result)
    assert "PASS" in md
    assert "| a |" in md and "| thin |" not in md    # the too-thin cell is excluded from ranking, not just ranked last
    path = sweep.write_outputs(cells, dsr, pbo_result, tmp_path)
    assert path.exists() and path.with_suffix(".json").exists()
