#!/usr/bin/env python3
"""NARMA-10 benchmark for the CRT-IR-ring reservoir.

NARMA-10 (Atiya & Parlos 2000; standard PRC benchmark) is a 10th-order
nonlinear autoregressive moving-average task. Given an i.i.d. input
u[t] ∈ [0, 0.5] (here we use the normalized gray-PRBS level index), the
target is:

    y[t+1] = 0.3·y[t] + 0.05·y[t]·Σ_{i=0..9} y[t-i] + 1.5·u[t-9]·u[t] + 0.1

A reservoir succeeds if a linear readout on its state can reconstruct y[t]
with low NMSE. NMSE = mean((y_true - y_pred)²) / var(y_true). Lower is
better; <0.1 is excellent, 0.2-0.4 is typical, ≥1.0 means worse than mean.

Usage:
    python tools/prc/narma10.py --run latest --window 16 --ridge 1e-4
    python tools/prc/narma10.py --run latest --window 8 --gpu
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
TMP_DIR = PROJECT_ROOT / "tmp" / "prc_runs"


def load_gprbs(path: Path) -> tuple[np.ndarray, np.ndarray]:
    """Return (lvl_idx in {0,1,2,3}, ADC value)."""
    lvls, adcs = [], []
    with path.open() as f:
        for line in f:
            if line.startswith("#") or line.startswith("tick"):
                continue
            parts = line.strip().split(",")
            if len(parts) >= 4:
                lvls.append(int(parts[1]))
                adcs.append(int(parts[3]))
    return np.array(lvls), np.array(adcs, dtype=float)


def narma10_target(u: np.ndarray) -> np.ndarray:
    """Generate the NARMA-10 target sequence from input u.

    u is expected in [0, 0.5]; we use the standard recurrence and seed the
    first 10 samples with y[0..9] = 0.1 (small constant). The sequence can
    saturate or blow up for some inputs — we clip y in [0, 1] for safety
    (standard practice in the PRC literature; see Goudarzi et al. 2014).
    """
    n = len(u)
    y = np.zeros(n)
    y[:10] = 0.1  # warm-up; first 10 samples are unreliable
    for t in range(9, n - 1):
        memory_sum = float(np.sum(y[t - 9 : t + 1]))
        y[t + 1] = 0.3 * y[t] + 0.05 * y[t] * memory_sum + 1.5 * u[t - 9] * u[t] + 0.1
        # Saturation guard
        if not np.isfinite(y[t + 1]) or y[t + 1] > 10.0:
            y[t + 1] = 10.0
        elif y[t + 1] < -10.0:
            y[t + 1] = -10.0
    return y


def build_state(adc: np.ndarray, window: int) -> np.ndarray:
    """Tapped-delay reservoir state: X[t, k] = adc[t - k]."""
    n = len(adc) - (window - 1)
    X = np.zeros((n, window))
    for k in range(window):
        X[:, k] = adc[(window - 1) - k : (window - 1) - k + n]
    return X


def add_polynomial_features(X: np.ndarray, degree: int = 2) -> np.ndarray:
    """Augment X with element-wise squares (and optionally cubes).

    NARMA-10 has a multiplicative u[t-9]·u[t] term; a purely linear readout
    on a tapped-delay state can't fit it. Adding x² features lets ridge
    regression approximate quadratic interactions through the bias term
    (correlation of x[i]² with cross-product terms).
    """
    if degree < 2:
        return X
    parts = [X]
    parts.append(X * X)  # element-wise squares
    if degree >= 3:
        parts.append(X * X * X)
    return np.hstack(parts)


def ridge_train_test(X: np.ndarray, y: np.ndarray, train_frac: float, ridge: float) -> dict:
    """Train ridge readout on first train_frac, test on the rest."""
    n = X.shape[0]
    n_tr = int(n * train_frac)
    X_tr, y_tr = X[:n_tr], y[:n_tr]
    X_te, y_te = X[n_tr:], y[n_tr:]

    # Standardize features (consistent with memory_capacity.py)
    mu_X = X_tr.mean(axis=0)
    sigma_X = X_tr.std(axis=0)
    sigma_X[sigma_X == 0] = 1.0
    X_tr_s = (X_tr - mu_X) / sigma_X
    X_te_s = (X_te - mu_X) / sigma_X

    mu_y = y_tr.mean()
    y_tr_c = y_tr - mu_y

    # Closed-form ridge
    A = X_tr_s.T @ X_tr_s + ridge * np.eye(X.shape[1])
    w = np.linalg.solve(A, X_tr_s.T @ y_tr_c)
    y_pred_tr = X_tr_s @ w + mu_y
    y_pred_te = X_te_s @ w + mu_y

    # Metrics
    def nmse(y_true, y_pred):
        v = float(np.var(y_true))
        if v == 0:
            return float("inf")
        return float(np.mean((y_true - y_pred) ** 2) / v)

    def r2(y_true, y_pred):
        ss_tot = float(np.sum((y_true - y_true.mean()) ** 2))
        if ss_tot == 0:
            return 0.0
        return 1.0 - float(np.sum((y_true - y_pred) ** 2) / ss_tot)

    return {
        "w": w,
        "n_train": n_tr,
        "n_test": n - n_tr,
        "nmse_train": nmse(y_tr, y_pred_tr),
        "nmse_test": nmse(y_te, y_pred_te),
        "r2_train": r2(y_tr, y_pred_tr),
        "r2_test": r2(y_te, y_pred_te),
        "y_test": y_te,
        "y_pred_test": y_pred_te,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", default=None)
    ap.add_argument(
        "--window",
        type=int,
        default=16,
        help="reservoir-state window (number of past ADC samples)",
    )
    ap.add_argument("--ridge", type=float, default=1e-4)
    ap.add_argument(
        "--poly-degree",
        type=int,
        default=1,
        choices=[1, 2, 3],
        help="polynomial expansion degree (1=linear, 2=add squares, 3=add cubes)",
    )
    ap.add_argument(
        "--train-frac", type=float, default=0.7, help="fraction of ticks used for training"
    )
    ap.add_argument(
        "--warmup",
        type=int,
        default=20,
        help="drop the first N ticks (NARMA target needs 10 to seed; +safety)",
    )
    args = ap.parse_args()

    if args.run:
        run_dir = TMP_DIR / args.run
    else:
        latest = TMP_DIR / "latest"
        run_dir = latest.resolve() if latest.is_symlink() else None
    if run_dir is None or not run_dir.exists():
        print("no run found", file=sys.stderr)
        sys.exit(1)

    gprbs_path = run_dir / "gprbs.csv"
    if not gprbs_path.exists():
        print("no gprbs.csv in run; NARMA-10 needs gray-PRBS input", file=sys.stderr)
        sys.exit(2)

    lvls, adc = load_gprbs(gprbs_path)
    n_ticks = len(lvls)

    # Normalize input to [0, 0.5] — standard NARMA range
    u = lvls.astype(float) / lvls.max() * 0.5
    y = narma10_target(u)

    # Drop warm-up samples from both target + ADC + input
    lvls = lvls[args.warmup :]
    adc = adc[args.warmup :]
    u = u[args.warmup :]
    y = y[args.warmup :]

    # Build state X (loses window-1 leading rows); align y/X
    X = build_state(adc, args.window)
    X = add_polynomial_features(X, args.poly_degree)
    # y[t] target corresponds to state X[t - (window-1)] — but build_state
    # already aligns so X[i] uses adc[(window-1) + i ... i]; we want to
    # predict y at the same index.
    y_aligned = y[args.window - 1 :]
    assert len(X) == len(y_aligned), f"shape mismatch: X={len(X)} y={len(y_aligned)}"

    print(f"# run: {run_dir.name}")
    print(f"# ticks: {n_ticks}  warm-up dropped: {args.warmup}  window: {args.window}")
    print(
        f"# effective samples (X, y): {len(X)} | features: {X.shape[1]} "
        f"(window={args.window}, poly_degree={args.poly_degree})"
    )
    print(f"# input u ∈ [{u.min():.3f}, {u.max():.3f}], target y var = {y.var():.4f}")

    result = ridge_train_test(X, y_aligned, args.train_frac, args.ridge)

    print("=" * 64)
    print(f"NARMA-10 (window={args.window}, ridge={args.ridge})")
    print("=" * 64)
    print(
        f"  train: n={result['n_train']:4d}  NMSE={result['nmse_train']:.4f}  R²={result['r2_train']:+.4f}"
    )
    print(
        f"  test : n={result['n_test']:4d}  NMSE={result['nmse_test']:.4f}  R²={result['r2_test']:+.4f}"
    )
    print("=" * 64)
    print("  Reference scale: NMSE<0.1 = excellent, 0.2-0.4 = typical PRC,")
    print("                   >1.0 = worse than mean prediction.")
    print("=" * 64)

    # Save predictions
    out_csv = run_dir / "narma10.csv"
    with out_csv.open("w") as f:
        w = csv.writer(f)
        w.writerow(["test_idx", "y_true", "y_pred"])
        for i, (y_t, y_p) in enumerate(zip(result["y_test"], result["y_pred_test"], strict=True)):
            w.writerow([i, f"{y_t:.6f}", f"{y_p:.6f}"])
        f.write(
            f"# nmse_test={result['nmse_test']:.6f} "
            f"r2_test={result['r2_test']:.6f} "
            f"window={args.window} ridge={args.ridge}\n"
        )
    print(f"# saved {out_csv.relative_to(PROJECT_ROOT)}")


if __name__ == "__main__":
    main()
