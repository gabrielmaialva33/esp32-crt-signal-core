#!/usr/bin/env python3
"""GPU bootstrap τ confidence intervals for the PRC decay fit.

Runs B=10000 bootstrap resamples in parallel on the GPU using batched
Levenberg-Marquardt-style nonlinear least squares. Reports percentile-based
CIs without assuming i.i.d. Gaussian residuals — addresses review issue I10
(curve_fit's pcov is optimistic on autocorrelated/heteroscedastic noise).

Models supported:
    exp:   y = A·exp(-t/τ) + B                       (3 params)
    biexp: y = A1·exp(-t/τ1) + A2·exp(-t/τ2) + B    (5 params)
    kww:   y = A·exp(-(t/τ)^β) + B                  (4 params, KWW stretched)

Implementation notes:
    * Optimization: batched torch.optim.LBFGS with line search on float64.
    * Bootstrap: residual bootstrap from the original fit residuals (preserves
      noise distribution shape).
    * Parallelism: each bootstrap is a row in a (B, 1) parameter tensor; we
      use vectorized residual evaluation so 10k fits hit the GPU as one op.

Usage:
    python tools/prc/gpu_bootstrap.py --run latest --model exp --B 10000
    python tools/prc/gpu_bootstrap.py --run latest --model biexp --B 5000
"""

from __future__ import annotations

import argparse
import csv
import sys
import time
from pathlib import Path

import numpy as np
import torch

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
TMP_DIR = PROJECT_ROOT / "tmp" / "prc_runs"


# ----------------------------------------------------------------------------
# Models — operate on tensors of shape (B, N_samples). Each model takes raw
# (unconstrained) params and reparameterizes inside the forward pass:
#   * τ > 0 enforced via softplus
#   * τ₂ > τ₁ in biexp enforced by parameterizing Δ = τ₂ − τ₁ > 0
#   * β ∈ (0.05, 1) in KWW enforced via sigmoid
# This lets LBFGS optimize in unbounded ℝⁿ without hitting numerical hells
# like β = 1e189 or τ < 0.
# ----------------------------------------------------------------------------
SOFTPLUS = torch.nn.functional.softplus
TAU_FLOOR = 1e-4  # lower bound on τ so the model never collapses to delta(t)


def _pos_tau(raw_slice: torch.Tensor) -> torch.Tensor:
    """τ > TAU_FLOOR via softplus — keeps fits away from degenerate τ=0."""
    return TAU_FLOOR + SOFTPLUS(raw_slice)


def transform_exp(raw: torch.Tensor) -> torch.Tensor:
    """raw=(A, τ_raw, B); returns (A, τ, B) with τ > 0 via softplus."""
    A = raw[:, 0:1]
    tau = _pos_tau(raw[:, 1:2])
    B = raw[:, 2:3]
    return torch.cat([A, tau, B], dim=1)


def model_exp(t: torch.Tensor, raw: torch.Tensor) -> torch.Tensor:
    p = transform_exp(raw)
    return p[:, 0:1] * torch.exp(-t / p[:, 1:2]) + p[:, 2:3]


def transform_biexp(raw: torch.Tensor) -> torch.Tensor:
    """raw=(A1, τ1_raw, A2, Δ_raw, B); returns (A1, τ1, A2, τ2, B) with τ1>floor,
    τ2 = τ1 + softplus(Δ_raw) > τ1."""
    A1 = raw[:, 0:1]
    tau1 = _pos_tau(raw[:, 1:2])
    A2 = raw[:, 2:3]
    tau2 = tau1 + SOFTPLUS(raw[:, 3:4]) + TAU_FLOOR
    B = raw[:, 4:5]
    return torch.cat([A1, tau1, A2, tau2, B], dim=1)


def model_biexp(t: torch.Tensor, raw: torch.Tensor) -> torch.Tensor:
    p = transform_biexp(raw)
    return p[:, 0:1] * torch.exp(-t / p[:, 1:2]) + p[:, 2:3] * torch.exp(-t / p[:, 3:4]) + p[:, 4:5]


def transform_kww(raw: torch.Tensor) -> torch.Tensor:
    """raw=(A, τ_raw, β_raw, B); returns (A, τ, β, B) with τ>floor, β∈(0.1, 1)."""
    A = raw[:, 0:1]
    tau = _pos_tau(raw[:, 1:2])
    beta = 0.1 + 0.9 * torch.sigmoid(raw[:, 2:3])
    B = raw[:, 3:4]
    return torch.cat([A, tau, beta, B], dim=1)


def model_kww(t: torch.Tensor, raw: torch.Tensor) -> torch.Tensor:
    """KWW stretched exponential. The +eps avoids the 0^β gradient singularity
    at t=0 (∂(t^β)/∂β = t^β·ln(t) → NaN when t=0)."""
    p = transform_kww(raw)
    eps = 1e-9
    return p[:, 0:1] * torch.exp(-(((t + eps) / p[:, 1:2]) ** p[:, 2:3])) + p[:, 3:4]


def transform_logistic(raw: torch.Tensor) -> torch.Tensor:
    """raw=(A, t0_raw, k_raw, B); returns (A, t0, k, B) with t0>floor, k>floor."""
    A = raw[:, 0:1]
    t0 = _pos_tau(raw[:, 1:2])
    k = _pos_tau(raw[:, 2:3])
    B = raw[:, 3:4]
    return torch.cat([A, t0, k, B], dim=1)


def model_logistic(t: torch.Tensor, raw: torch.Tensor) -> torch.Tensor:
    """Logistic (sigmoid) decay: y(t) = A / (1 + exp((t - t0)/k)) + B.

    Captures the delayed-onset decay where the system stays at LIGHT for a
    plateau, then transitions through an inflection at t0, then settles to B.
    Inflection-point slope is −A/(4k). The 10–90% transition width is ~4.39k.
    """
    p = transform_logistic(raw)
    A, t0, k, B = p[:, 0:1], p[:, 1:2], p[:, 2:3], p[:, 3:4]
    return A / (1.0 + torch.exp((t - t0) / k)) + B


# Map of (forward_fn, n_raw_params, transform_fn, param_names_in_real_space)
MODELS = {
    "exp": (model_exp, 3, transform_exp, ("A", "tau", "B")),
    "biexp": (model_biexp, 5, transform_biexp, ("A1", "tau1", "A2", "tau2", "B")),
    "kww": (model_kww, 4, transform_kww, ("A", "tau", "beta", "B")),
    "logistic": (model_logistic, 4, transform_logistic, ("A", "t0", "k", "B")),
}


def inverse_softplus(y: float) -> float:
    """Inverse of softplus(x) = log(1 + exp(x))."""
    return float(np.log(np.expm1(max(y, 1e-12))))


def inverse_sigmoid_beta(beta: float) -> float:
    """Inverse of the β reparameterization in transform_kww (β ∈ (0.1, 1))."""
    u = max(min((beta - 0.1) / 0.9, 1 - 1e-9), 1e-9)
    return float(np.log(u / (1 - u)))


def make_p0(
    model_name: str, A0: float, B0: float, tau0: float, device: torch.device
) -> torch.Tensor:
    """Build raw initial guess (in unconstrained optimization space).

    For biexp the fast/slow split assumes phosphor + electronics produce two
    well-separated time constants. Empirically the fast component on this CRT
    is in the 20–80 ms range and the slow tail is in the 300–500 ms range —
    seed accordingly. A wider τ₁/τ₂ separation in p0 prevents the optimizer
    from collapsing to a single-exp degenerate solution.
    """
    if model_name == "exp":
        raw = [A0, inverse_softplus(tau0), B0]
    elif model_name == "biexp":
        # Fast: τ1 ≈ 40 ms with 25% of amplitude; Slow: τ2 ≈ tau0 with 75%.
        tau1_guess = 0.04
        tau2_guess = max(tau0, 0.3)
        delta_guess = tau2_guess - tau1_guess
        raw = [
            0.25 * A0,
            inverse_softplus(tau1_guess),
            0.75 * A0,
            inverse_softplus(delta_guess),
            B0,
        ]
    elif model_name == "kww":
        raw = [A0, inverse_softplus(tau0), inverse_sigmoid_beta(0.7), B0]
    elif model_name == "logistic":
        # t0 ≈ tau0 (midpoint), k ≈ tau0/4 (transition width)
        raw = [A0, inverse_softplus(tau0), inverse_softplus(tau0 / 4), B0]
    else:
        raise ValueError(model_name)
    return torch.tensor([raw], dtype=torch.float64, device=device)


# ----------------------------------------------------------------------------
# Single batched fit on GPU (LBFGS with line-search)
# ----------------------------------------------------------------------------
def batched_fit(
    t: torch.Tensor,
    y: torch.Tensor,  # shape (B, N)
    model_name: str,
    p0_raw: torch.Tensor,  # shape (B, n_raw_params) in unconstrained space
    max_iter: int = 200,
    lr: float = 0.5,
    adam_warmup: int = 200,
) -> torch.Tensor:
    """Fit `model_name` to each of B datasets in parallel.

    Strategy: short Adam warmup (robust to ill-conditioning, small steps) then
    LBFGS with strong-Wolfe line search for high-precision convergence. KWW
    in particular benefits from the warmup — direct LBFGS on the unconditioned
    KWW landscape blows up.

    Returns the final RAW parameter tensor; apply MODELS[m][2] to map back
    to real-space.
    """
    model_fn = MODELS[model_name][0]
    raw = p0_raw.clone().requires_grad_(True)

    # Adam warmup — stable but slow convergence. Acts as a regularizer.
    adam = torch.optim.Adam([raw], lr=0.05)
    for _ in range(adam_warmup):
        adam.zero_grad()
        y_pred = model_fn(t, raw)
        loss = ((y - y_pred) ** 2).sum()
        if not torch.isfinite(loss):
            break
        loss.backward()
        # Gradient clipping — prevents huge KWW spikes during warmup.
        torch.nn.utils.clip_grad_norm_([raw], max_norm=1e3)
        adam.step()

    # LBFGS refinement — converges fast once we're in the basin of attraction.
    lbfgs = torch.optim.LBFGS(
        [raw],
        lr=lr,
        max_iter=max_iter,
        line_search_fn="strong_wolfe",
        tolerance_grad=1e-9,
        tolerance_change=1e-12,
    )

    def closure():
        lbfgs.zero_grad()
        y_pred = model_fn(t, raw)
        residual = y - y_pred
        loss = (residual * residual).sum()
        if not torch.isfinite(loss):
            # Return a large finite loss to keep LBFGS from poisoning state.
            return torch.tensor(1e30, dtype=loss.dtype, device=loss.device)
        loss.backward()
        return loss

    lbfgs.step(closure)
    return raw.detach()


# ----------------------------------------------------------------------------
# Residual bootstrap on GPU
# ----------------------------------------------------------------------------
def residual_bootstrap(
    t_np: np.ndarray,
    y_np: np.ndarray,
    model_name: str,
    n_bootstrap: int,
    device: torch.device,
    seed: int = 12345,
) -> dict:
    """Generate B residual-bootstrap fits in parallel on GPU.

    Steps:
        1. Initial fit to (t, y) gives baseline params and residuals r[i].
        2. For each bootstrap b=1..B, resample residuals with replacement
           and add to the baseline prediction → synthetic y*[b].
        3. Re-fit each y*[b] in parallel using batched LBFGS.

    Returns dict with:
        baseline: (n_params,) baseline fit
        bootstrap: (B, n_params) bootstrap fits
        ci_lo, ci_hi: 2.5 / 97.5 percentile arrays of shape (n_params,)
        ci_50: median
        mean, std: across bootstrap
    """
    rng = torch.Generator(device=device).manual_seed(seed)
    model_fn, _, transform_fn, names = MODELS[model_name]

    t = torch.tensor(t_np, dtype=torch.float64, device=device)
    y = torch.tensor(y_np, dtype=torch.float64, device=device)
    n_samples = len(t)

    # Initial guess from data — heuristic, then mapped to raw space
    A0 = float(y_np[0] - y_np[-1])
    B0 = float(y_np[-1])
    tau0 = max((t_np[-1] - t_np[0]) / 5, 1e-3)
    p0_raw = make_p0(model_name, A0, B0, tau0, device)

    # ---- Step 1: baseline fit (B=1) — raw params
    baseline_raw = batched_fit(t.unsqueeze(0), y.unsqueeze(0), model_name, p0_raw)
    baseline_pred = model_fn(t.unsqueeze(0), baseline_raw).squeeze(0)
    residuals = (y - baseline_pred).cpu().numpy()

    # ---- Step 2: build B synthetic datasets by residual bootstrap
    res_t = torch.tensor(residuals, dtype=torch.float64, device=device)
    idx = torch.randint(0, n_samples, (n_bootstrap, n_samples), generator=rng, device=device)
    res_resampled = res_t[idx]
    y_boot = baseline_pred.unsqueeze(0) + res_resampled

    # ---- Step 3: batched fit — share baseline raw as p0 for all bootstrap rows
    p0_batch = baseline_raw.expand(n_bootstrap, -1).clone()
    t_batch = t.unsqueeze(0).expand(n_bootstrap, -1)
    boot_raw = batched_fit(t_batch, y_boot, model_name, p0_batch)

    # ---- Step 4: transform RAW → REAL space, then take percentiles in real space
    baseline_real = transform_fn(baseline_raw).cpu().numpy().squeeze(0)
    boot_real = transform_fn(boot_raw).cpu().numpy()
    ci_lo = np.percentile(boot_real, 2.5, axis=0)
    ci_50 = np.percentile(boot_real, 50, axis=0)
    ci_hi = np.percentile(boot_real, 97.5, axis=0)
    mean = boot_real.mean(axis=0)
    std = boot_real.std(axis=0)

    return {
        "model": model_name,
        "names": names,
        "baseline": baseline_real,
        "bootstrap": boot_real,
        "ci_lo": ci_lo,
        "ci_50": ci_50,
        "ci_hi": ci_hi,
        "mean": mean,
        "std": std,
        "n_bootstrap": n_bootstrap,
    }


# ----------------------------------------------------------------------------
# Decay CSV loader (matches firmware/capture format)
# ----------------------------------------------------------------------------
def load_decay(run_dir: Path) -> dict:
    decay_path = run_dir / "decay.csv"
    if not decay_path.exists():
        raise FileNotFoundError(f"{decay_path} not found")

    rows = []
    header = None
    with decay_path.open() as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            if header is None:
                header = line.strip().split(",")
                continue
            rows.append(line.strip().split(","))
    arr = np.array(rows)
    # Columns: sample_idx,t_us,phase,trial0,trial1,trial2,...
    t_us = arr[:, 1].astype(float)
    phase = arr[:, 2]
    trials = arr[:, 3:].astype(float)
    mask = phase == "DECAY"
    if not mask.any():
        raise ValueError("no DECAY rows in decay.csv")
    t_s = (t_us[mask] - t_us[mask][0]) / 1e6
    return {"t_s": t_s, "trials": trials[mask], "header": header}


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
def format_param(name: str, baseline: float, lo: float, hi: float, units: str = "") -> str:
    # Use ms for tau* params, raw for others.
    if name.startswith("tau") and units == "":
        baseline_ms = baseline * 1000
        lo_ms = lo * 1000
        hi_ms = hi * 1000
        return f"  {name:>6s} = {baseline_ms:9.2f} ms   [95% CI: {lo_ms:8.2f} – {hi_ms:8.2f}]"
    return f"  {name:>6s} = {baseline:12.4f}     [95% CI: {lo:12.4f} – {hi:12.4f}]"


def write_results_csv(out_path: Path, all_results: list[dict]) -> None:
    with out_path.open("w") as f:
        w = csv.writer(f)
        w.writerow(
            ["trial", "model", "param", "baseline", "ci_lo", "ci_50", "ci_hi", "mean", "std"]
        )
        for r in all_results:
            for i, name in enumerate(r["names"]):
                w.writerow(
                    [
                        r["trial"],
                        r["model"],
                        name,
                        f"{r['baseline'][i]:.6e}",
                        f"{r['ci_lo'][i]:.6e}",
                        f"{r['ci_50'][i]:.6e}",
                        f"{r['ci_hi'][i]:.6e}",
                        f"{r['mean'][i]:.6e}",
                        f"{r['std'][i]:.6e}",
                    ]
                )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", default=None)
    ap.add_argument("--model", choices=list(MODELS.keys()), default="exp")
    ap.add_argument("--bootstrap", "-B", type=int, default=10000)
    ap.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    ap.add_argument("--seed", type=int, default=12345)
    args = ap.parse_args()

    if args.run:
        run_dir = TMP_DIR / args.run
    else:
        latest = TMP_DIR / "latest"
        run_dir = latest.resolve() if latest.is_symlink() else None
    if run_dir is None or not run_dir.exists():
        print("no run found", file=sys.stderr)
        sys.exit(1)

    device = torch.device(args.device)
    print(f"# device: {device}")
    if device.type == "cuda":
        print(f"# gpu: {torch.cuda.get_device_name(0)}")
    print(f"# model: {args.model}  bootstrap: {args.bootstrap}")

    decay = load_decay(run_dir)
    print(f"# loaded decay.csv: {decay['trials'].shape[1]} trials × {len(decay['t_s'])} samples")

    all_results = []
    for trial_idx in range(decay["trials"].shape[1]):
        y = decay["trials"][:, trial_idx]
        if np.all(y == 0):
            continue
        t0 = time.time()
        result = residual_bootstrap(
            decay["t_s"], y, args.model, args.bootstrap, device, args.seed + trial_idx
        )
        elapsed = time.time() - t0

        result["trial"] = trial_idx
        all_results.append(result)

        print("=" * 64)
        print(
            f"Trial {trial_idx} — {result['n_bootstrap']:,} bootstrap fits in "
            f"{elapsed:.2f}s on {device}"
        )
        print("=" * 64)
        for i, name in enumerate(result["names"]):
            print(
                format_param(
                    name,
                    result["baseline"][i],
                    result["ci_lo"][i],
                    result["ci_hi"][i],
                )
            )

    out_csv = run_dir / f"bootstrap_{args.model}.csv"
    write_results_csv(out_csv, all_results)
    print("=" * 64)
    print(f"# saved {out_csv.relative_to(PROJECT_ROOT)}")

    # Cross-trial summary of the primary time parameter for each model.
    primary_time_idx = {"exp": 1, "biexp": 1, "kww": 1, "logistic": 1}
    primary_name = {"exp": "τ", "biexp": "τ₁", "kww": "τ", "logistic": "t0"}
    idx = primary_time_idx.get(args.model)
    label = primary_name.get(args.model, "param[1]")
    if idx is not None and all_results:
        vals_ms = [r["baseline"][idx] * 1000 for r in all_results]
        print("=" * 64)
        print(
            f"{label} baseline across {len(vals_ms)} trial(s): "
            f"{np.mean(vals_ms):.2f} ± {np.std(vals_ms):.2f} ms"
        )


if __name__ == "__main__":
    main()
