#!/usr/bin/env python3
"""Memory Capacity benchmark of the CRT-IR-ring reservoir.

Standard PRC benchmark (Jaeger 2002): for each delay k, train a linear
readout to predict u[t-k] from the reservoir state x[t]. The R² of the
prediction is MC[k]; the total Memory Capacity is MC = Σ_k MC[k].

State construction from the available data:
  * The PRBS log gives one ADC value per bit.
  * To build a richer reservoir state, we use a sliding window of the
    last W ADC samples — the reservoir's "fading memory" is then probed
    by training W weights to reconstruct u[t-k].

A high MC means the system stores past inputs in a linearly recoverable
form. For an exponential reservoir with τ ≫ bit-width, MC saturates to
W (the state dimensionality) when k is small.

Outputs:
  mc.csv     — k, MC(k)
  mc_fit.txt — total MC + per-delay table

Usage:
  python tools/prc_memory_capacity.py            # latest run
  python tools/prc_memory_capacity.py --window 8 # readout window size
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
TMP_DIR = PROJECT_ROOT / "tmp" / "prc_runs"


def load_prbs(path: Path):
    bits, adc = [], []
    with path.open() as f:
        for line in f:
            if line.startswith("#") or line.startswith("bit_idx"):
                continue
            parts = line.strip().split(",")
            if len(parts) >= 3:
                bits.append(int(parts[1]))
                adc.append(int(parts[2]))
    return np.array(bits), np.array(adc, dtype=float)


def load_mprbs(path: Path):
    """Load 4-quadrant multi-PRBS. Returns (bits matrix Nx4, adc array N)."""
    rows = []
    with path.open() as f:
        for line in f:
            if line.startswith("#") or line.startswith("tick"):
                continue
            parts = line.strip().split(",")
            if len(parts) >= 7:
                rows.append(
                    [int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]), int(parts[6])]
                )
    arr = np.array(rows)
    return arr[:, :4], arr[:, 4].astype(float)


def build_state_matrix(adc: np.ndarray, window: int) -> np.ndarray:
    """X[t] = [adc[t], adc[t-1], ..., adc[t-window+1]] for t >= window-1."""
    n = len(adc) - (window - 1)
    X = np.zeros((n, window))
    for k in range(window):
        X[:, k] = adc[(window - 1) - k : (window - 1) - k + n]
    return X


def mc_at_delay(bits: np.ndarray, X: np.ndarray, k: int, ridge: float) -> tuple[float, float]:
    """Compute Jaeger memory capacity at delay k: MC_k = corr²(u[t-k], û_k(t)).

    Trains a ridge readout on a 70/30 sequential split, then reports the
    squared Pearson correlation between the held-out target and prediction.
    For an unbiased OLS predictor this equals R² of the held-out set; for
    ridge with λ > 0 (biased predictor), corr² > R², and corr² is the
    quantity Jaeger (2002) defines as memory capacity.
    """
    n = X.shape[0]
    window = X.shape[1]
    t_offsets = np.arange(window - 1, window - 1 + n)
    valid = t_offsets - k >= 0
    if valid.sum() < 10:
        return 0.0, 0.0
    Xv = X[valid]
    target_idx = t_offsets[valid] - k
    yv = bits[target_idx].astype(float)
    if yv.var() == 0:
        return 0.0, 0.0
    # Train/test split — first 70% train, last 30% test
    n_tr = int(len(Xv) * 0.7)
    Xt, yt = Xv[:n_tr], yv[:n_tr]
    Xv2, yv2 = Xv[n_tr:], yv[n_tr:]
    # Standardize features (ridge penalty is scale-dependent; columns at
    # different lags carry different variance, so we want unit-variance
    # features under L2 penalty).
    mu_X = Xt.mean(axis=0)
    sigma_X = Xt.std(axis=0)
    sigma_X[sigma_X == 0] = 1.0
    Xt_c = (Xt - mu_X) / sigma_X
    Xv_c = (Xv2 - mu_X) / sigma_X
    mu_y = yt.mean()
    yt_c = yt - mu_y
    # Ridge regression closed form
    A = Xt_c.T @ Xt_c + ridge * np.eye(window)
    w = np.linalg.solve(A, Xt_c.T @ yt_c)
    y_pred = Xv_c @ w + mu_y
    # Jaeger MC: squared Pearson correlation between target and prediction.
    if np.std(y_pred) == 0:
        return 0.0, float(yv.var())
    corr = np.corrcoef(yv2, y_pred)[0, 1]
    mc_k = float(corr * corr) if np.isfinite(corr) else 0.0
    return mc_k, float(yv.var())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", default=None)
    ap.add_argument(
        "--window", type=int, default=8, help="readout window size (number of past ADC samples)"
    )
    ap.add_argument("--max-delay", type=int, default=15)
    ap.add_argument("--min-delay", type=int, default=1, help="start delay (Jaeger: k>=1)")
    ap.add_argument(
        "--include-zero",
        action="store_true",
        help="include k=0 (feedthrough) in total MC — non-standard",
    )
    ap.add_argument("--ridge", type=float, default=1e-3)
    args = ap.parse_args()

    if args.run:
        run_dir = TMP_DIR / args.run
    else:
        latest = TMP_DIR / "latest"
        run_dir = latest.resolve() if latest.is_symlink() else None
        if run_dir is None:
            runs = sorted([p for p in TMP_DIR.iterdir() if p.is_dir()])
            run_dir = runs[-1] if runs else None
    if run_dir is None or not run_dir.exists():
        print("no run found", file=sys.stderr)
        sys.exit(1)

    print(f"# analyzing {run_dir}")

    # Prefer gprbs.csv if present
    gprbs_path = run_dir / "gprbs.csv"
    if gprbs_path.exists():
        lvls = []
        adcs = []
        with gprbs_path.open() as f:
            for line in f:
                if line.startswith("#") or line.startswith("tick"):
                    continue
                parts = line.strip().split(",")
                if len(parts) >= 4:
                    lvls.append(int(parts[1]))
                    adcs.append(int(parts[3]))
        lvls = np.array(lvls, dtype=float)
        adc = np.array(adcs, dtype=float)
        print(f"# gray-level PRBS: {len(lvls)} ticks, ADC range {adc.min():.0f}..{adc.max():.0f}")
        X = build_state_matrix(adc, args.window)
        k_start = 0 if args.include_zero else args.min_delay
        k_mc = []
        for k in range(k_start, args.max_delay + 1):
            mc_k, _ = mc_at_delay(lvls, X, k, args.ridge)
            k_mc.append((k, mc_k))
        total_mc = sum(mc for _, mc in k_mc)
        rank_X = int(np.linalg.matrix_rank(X))
        bound = min(rank_X, args.window)
        print("=" * 64)
        print("GRAY-LEVEL PRBS — predict u[t-k] (level idx 0..3) from x[t]")
        print(
            f"  window W = {args.window}, ridge λ = {args.ridge}, k = {k_start}..{args.max_delay}"
        )
        print("=" * 64)
        for k, mc in k_mc:
            bar = "#" * int(mc * 40)
            print(f"  k={k:2d} | {mc:.4f}  {bar}")
        print("=" * 64)
        print(f"  total MC (corr²) = {total_mc:.4f}")
        print(
            f"  upper bound = min(rank(X)={rank_X}, W={args.window}) = {bound};"
            f" ratio = {100 * total_mc / bound:.1f}%"
        )
        print("=" * 64)

        out_csv = run_dir / "mc_gray.csv"
        with out_csv.open("w") as f:
            f.write("delay_k,mc\n")
            for k, mc in k_mc:
                f.write(f"{k},{mc:.6f}\n")
            f.write(
                f"# total_mc={total_mc:.6f} window={args.window} "
                f"rank_X={rank_X} ridge={args.ridge} k_start={k_start}\n"
            )
        print(f"# saved {out_csv.relative_to(PROJECT_ROOT)}")
        return

    mprbs_path = run_dir / "mprbs.csv"
    if mprbs_path.exists():
        bits_matrix, adc = load_mprbs(mprbs_path)
        print(
            f"# multi-input mode: {bits_matrix.shape[0]} ticks × {bits_matrix.shape[1]} quadrants"
        )
        X = build_state_matrix(adc, args.window)
        k_start = 0 if args.include_zero else args.min_delay
        k_range = range(k_start, args.max_delay + 1)
        rank_X = int(np.linalg.matrix_rank(X))
        bound = min(rank_X, args.window)
        total_mc_global = 0.0
        per_quadrant = []  # per_quadrant[q] = [(k, mc), ...]
        for q in range(bits_matrix.shape[1]):
            bits_q = bits_matrix[:, q]
            k_mc = []
            for k in k_range:
                mc_k, _ = mc_at_delay(bits_q, X, k, args.ridge)
                k_mc.append((k, mc_k))
            tot = sum(mc for _, mc in k_mc)
            per_quadrant.append(k_mc)
            total_mc_global += tot
            print(f"\n--- quadrant {q} (TL=0,TR=1,BL=2,BR=3) — MC = {tot:.4f} ---")
            for k, mc in k_mc:
                bar = "#" * int(mc * 40)
                print(f"  k={k:2d} | {mc:.4f}  {bar}")
        print("\n" + "=" * 64)
        print(f"  total MC across {bits_matrix.shape[1]} channels = {total_mc_global:.4f}")
        # Note: parallel tasks share the same state X; upper bound is rank(X),
        # NOT n_channels * W. The 4·W bound is wrong because the 4 tasks
        # cannot collectively exceed the state's effective dimensionality.
        print(f"  upper bound (shared state) = min(rank(X)={rank_X}, W={args.window}) = {bound}")
        print(f"  ratio = {100 * total_mc_global / bound:.1f}% of bound")
        print("=" * 64)

        out_csv = run_dir / "mc_multi.csv"
        with out_csv.open("w") as f:
            f.write("delay_k," + ",".join(f"q{q}" for q in range(len(per_quadrant))) + "\n")
            for idx, k in enumerate(k_range):
                row = ",".join(f"{per_quadrant[q][idx][1]:.6f}" for q in range(len(per_quadrant)))
                f.write(f"{k},{row}\n")
            f.write(
                f"# total_mc={total_mc_global:.6f} window={args.window} "
                f"rank_X={rank_X} ridge={args.ridge} k_start={k_start}\n"
            )
        print(f"# saved {out_csv.relative_to(PROJECT_ROOT)}")
        return

    bits, adc = load_prbs(run_dir / "prbs.csv")
    if len(bits) < args.window + args.max_delay + 10:
        print(
            f"# only {len(bits)} bits — need at least {args.window + args.max_delay + 10}",
            file=sys.stderr,
        )
        sys.exit(2)

    X = build_state_matrix(adc, args.window)
    k_start = 0 if args.include_zero else args.min_delay
    rank_X = int(np.linalg.matrix_rank(X))
    bound = min(rank_X, args.window)
    k_mc = []
    for k in range(k_start, args.max_delay + 1):
        mc_k, _ = mc_at_delay(bits, X, k, args.ridge)
        k_mc.append((k, mc_k))
    total_mc = sum(mc for _, mc in k_mc)

    # Output table
    print("=" * 64)
    print(f"MEMORY CAPACITY of CRT-IR reservoir  (n_bits={len(bits)})")
    print(f"  window W = {args.window}  ridge λ = {args.ridge}  k = {k_start}..{args.max_delay}")
    print("=" * 64)
    print("  delay k | MC(k) = corr²(u[t-k], û[t-k] | x[t])")
    print("  --------+--------------------------------------")
    for k, mc in k_mc:
        bar = "#" * int(mc * 40)
        print(f"  {k:7d} | {mc:.4f}  {bar}")
    print("  --------+--------------------------------------")
    print(f"   total  | MC = Σ_k MC(k) = {total_mc:.4f}")
    print(f"   bound  | min(rank(X)={rank_X}, W={args.window}) = {bound}")
    print(f"   ratio  | {100 * total_mc / bound:.1f}% of bound")
    print("=" * 64)

    # Save CSV
    out_csv = run_dir / "mc.csv"
    with out_csv.open("w") as f:
        f.write("delay_k,mc\n")
        for k, mc in k_mc:
            f.write(f"{k},{mc:.6f}\n")
        f.write(
            f"# total_mc={total_mc:.6f} window={args.window} "
            f"rank_X={rank_X} ridge={args.ridge} k_start={k_start}\n"
        )
    print(f"# saved {out_csv.relative_to(PROJECT_ROOT)}")


if __name__ == "__main__":
    main()
