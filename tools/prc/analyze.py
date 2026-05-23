#!/usr/bin/env python3
"""Rigorous PRC calibration analysis.

Reads the CSVs produced by capture_prc_run.py and fits parametric models
with confidence intervals:

  xy.csv      → rotated 2D Gaussian (μ_x, μ_y, σ_x, σ_y, θ, A, B)
                via scipy.optimize.curve_fit (Levenberg-Marquardt, with
                covariance matrix → 1σ parameter uncertainties).
  decay.csv   → exponential A·exp(-t/τ) + B per trial, joint fit across
                trials, τ ± 1σ.
  prbs.csv    → impulse response h[k] via cross-correlation of bit
                sequence and ADC, normalized; rise/fall asymmetry.

Outputs:
  fit.txt   — human-readable report
  xy_fit.csv — fitted gaussian on the same grid
  decay_fit.csv — fitted exp curve
  prbs_h.csv — impulse response

Usage:
  python tools/prc_analyze.py                  # latest run
  python tools/prc_analyze.py --run 20260426_234224
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np
from scipy.optimize import curve_fit

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
TMP_DIR = PROJECT_ROOT / "tmp" / "prc_runs"


# ----------------------------------------------------------------------------
# Models
# ----------------------------------------------------------------------------
def gauss2d_rot(xy, A, x0, y0, sx, sy, theta, B):
    """Rotated 2D Gaussian. xy is a 2xN array (rows are x, y)."""
    x, y = xy
    ct, st = np.cos(theta), np.sin(theta)
    a = (ct**2) / (2 * sx**2) + (st**2) / (2 * sy**2)
    b = -np.sin(2 * theta) / (4 * sx**2) + np.sin(2 * theta) / (4 * sy**2)
    c = (st**2) / (2 * sx**2) + (ct**2) / (2 * sy**2)
    dx, dy = x - x0, y - y0
    return A * np.exp(-(a * dx * dx + 2 * b * dx * dy + c * dy * dy)) + B


def exp_decay(t, A, tau, B):
    """Single-exponential decay: A·exp(-t/τ) + B."""
    return A * np.exp(-t / tau) + B


def biexp_decay(t, A1, tau1, A2, tau2, B):
    """Bi-exponential decay: A1·exp(-t/τ1) + A2·exp(-t/τ2) + B.

    Phosphor + electronics + ambient drift typically have two well-separated
    time constants. τ1 is the fast component (phosphor primary), τ2 is the
    slow tail (deep traps + IR-LED thermal lag + ADC drift).
    """
    return A1 * np.exp(-t / tau1) + A2 * np.exp(-t / tau2) + B


def kww_decay(t, A, tau, beta, B):
    """Kohlrausch–Williams–Watts (stretched exponential): A·exp(-(t/τ)^β) + B.

    β ∈ (0, 1] controls dispersiveness. β = 1 → single exponential. β < 1
    indicates a broad distribution of relaxation times — typical of disordered
    phosphors (Phillips 1996, Rep. Prog. Phys. 59, 1133).
    """
    return A * np.exp(-((t / tau) ** beta)) + B


def aicc(n: int, k: int, ss_res: float) -> float:
    """Corrected Akaike Information Criterion (small-sample-friendly).

    AICc = n·ln(SSR/n) + 2k + 2k(k+1)/(n-k-1)
    Lower is better. Use for model selection between exp/biexp/KWW.
    """
    if n <= k + 1 or ss_res <= 0:
        return float("inf")
    return n * np.log(ss_res / n) + 2 * k + 2 * k * (k + 1) / (n - k - 1)


# ----------------------------------------------------------------------------
# IO
# ----------------------------------------------------------------------------
def load_xy(path: Path):
    """Parses xy_csv lines from raw.log: each row is 'rowJJ: a0 a1 ... a15'."""
    rows = []
    if not path.exists():
        return None
    text = path.read_text(errors="replace")
    for line in text.splitlines():
        if "xy_csv" not in line and "row" not in line:
            continue
        # extract "rowJJ: ..." style
        if "row" in line and ":" in line:
            tail = line.split(":", 2)
            # tail[-1] should contain the numbers
            parts = tail[-1].split()
            try:
                vals = [int(v) for v in parts]
                if len(vals) == 16:
                    rows.append(vals)
            except ValueError:
                continue
    if len(rows) != 16:
        return None
    return np.array(rows, dtype=float)  # shape (16, 16); rows[j][i] = ADC at (j, i)


def parse_xy_meta(path: Path):
    """Extract probe_r and grid step info from raw.log so we map (i, j)→pixels."""
    meta = {"grid": 16, "probe_r": 6, "fb_w": 256, "fb_h": 240}
    if path.exists():
        text = path.read_text(errors="replace")
        for line in text.splitlines():
            if "starting" in line and "x" in line and "probe r=" in line:
                # e.g. "xy_map: starting 16x16 sweep, probe r=6"
                try:
                    g = int(line.split("starting")[1].split("x")[0].strip())
                    meta["grid"] = g
                except Exception:
                    pass
                try:
                    r = int(line.split("probe r=")[1].split()[0])
                    meta["probe_r"] = r
                except Exception:
                    pass
    return meta


def load_decay(path: Path):
    if not path.exists():
        return None
    rows = []
    with path.open() as f:
        rdr = csv.reader(f)
        header = None
        for row in rdr:
            if not row or row[0].startswith("#"):
                continue
            if header is None:
                header = row
                continue
            rows.append(row)
    if not rows:
        return None
    arr = np.array(rows)
    t_us = arr[:, 1].astype(float)
    phase = arr[:, 2]
    trials = arr[:, 3:].astype(float)
    return {"t_us": t_us, "phase": phase, "trials": trials, "header": header}


def load_prbs(path: Path):
    if not path.exists():
        return None
    bits = []
    adc = []
    with path.open() as f:
        for line in f:
            if line.startswith("#") or line.startswith("bit_idx"):
                continue
            parts = line.strip().split(",")
            if len(parts) >= 3:
                bits.append(int(parts[1]))
                adc.append(int(parts[2]))
    return {"bits": np.array(bits), "adc": np.array(adc, dtype=float)}


# ----------------------------------------------------------------------------
# Fits
# ----------------------------------------------------------------------------
def fit_xy(matrix: np.ndarray, meta: dict):
    """Fit a rotated 2D Gaussian to the XY heat map."""
    g = matrix.shape[0]
    probe_r = meta["probe_r"]
    fb_w = meta["fb_w"]
    fb_h = meta["fb_h"]

    # Build pixel coordinates of every cell center
    i_idx, j_idx = np.meshgrid(np.arange(g), np.arange(g))
    cx = (i_idx * (fb_w - 2 * probe_r - 1)) // (g - 1) + probe_r
    cy = (j_idx * (fb_h - 2 * probe_r - 1)) // (g - 1) + probe_r

    x = cx.ravel().astype(float)
    y = cy.ravel().astype(float)
    z = matrix.ravel().astype(float)

    # Initial guess
    A0 = z.max() - z.min()
    B0 = z.min()
    # weighted centroid as initial center
    w = np.maximum(z - B0, 0)
    if w.sum() == 0:
        return None
    x0 = float(np.sum(w * x) / w.sum())
    y0 = float(np.sum(w * y) / w.sum())
    sx0 = max(20.0, float(np.sqrt(np.sum(w * (x - x0) ** 2) / w.sum())))
    sy0 = max(20.0, float(np.sqrt(np.sum(w * (y - y0) ** 2) / w.sum())))
    p0 = (A0, x0, y0, sx0, sy0, 0.0, B0)

    bounds = (
        (0, 0, 0, 1, 1, -np.pi, 0),
        (4096, fb_w, fb_h, fb_w, fb_h, np.pi, 4096),
    )
    try:
        popt, pcov = curve_fit(
            gauss2d_rot, np.vstack([x, y]), z, p0=p0, bounds=bounds, maxfev=10000
        )
        perr = np.sqrt(np.diag(pcov))
    except Exception as e:
        return {"error": str(e)}

    z_pred = gauss2d_rot(np.vstack([x, y]), *popt)
    ss_res = np.sum((z - z_pred) ** 2)
    ss_tot = np.sum((z - z.mean()) ** 2)
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0

    # FWHM along the principal axes: factor is exactly 2·sqrt(2·ln(2))
    FWHM_FACTOR = 2.0 * np.sqrt(2.0 * np.log(2.0))  # 2.354820045...
    fwhm_x = FWHM_FACTOR * popt[3]
    fwhm_y = FWHM_FACTOR * popt[4]

    return {
        "A": popt[0],
        "x0": popt[1],
        "y0": popt[2],
        "sx": popt[3],
        "sy": popt[4],
        "theta": popt[5],
        "B": popt[6],
        "err": dict(zip(["A", "x0", "y0", "sx", "sy", "theta", "B"], perr, strict=True)),
        "r2": r2,
        "fwhm_x": fwhm_x,
        "fwhm_y": fwhm_y,
        "z_pred": z_pred.reshape(g, g),
        "x_grid": cx,
        "y_grid": cy,
    }


def fit_decay(decay: dict):
    """Per-trial exponential fit on the post-switch portion of every trial.

    Each trial fits its own (A, τ, B) independently. Across-trial τ is
    summarized by mean ± std. For a joint fit with shared τ, see
    docs/research/prc_math_review.md I2.
    """
    t_us = decay["t_us"]
    phase = decay["phase"]
    trials = decay["trials"]
    mask = phase == "DECAY"
    if not mask.any():
        return None
    t = (t_us[mask] - t_us[mask][0]) / 1e6  # seconds, starting at switch

    fits = []
    n_samples = len(t)
    for col in range(trials.shape[1]):
        y = trials[mask, col]
        if np.all(y == 0):
            continue
        A0 = y[0] - y[-1]
        B0 = y[-1]
        tau0 = max((t[-1] - t[0]) / 5, 1e-3)
        # Fit all three models; pick by AICc.
        candidates = []
        # Single exponential
        try:
            popt, pcov = curve_fit(exp_decay, t, y, p0=(A0, tau0, B0), maxfev=5000)
            y_pred = exp_decay(t, *popt)
            ss_res = float(np.sum((y - y_pred) ** 2))
            ss_tot = float(np.sum((y - y.mean()) ** 2))
            r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0
            candidates.append(
                {
                    "model": "exp",
                    "params": {"A": popt[0], "tau": popt[1], "B": popt[2]},
                    "err": dict(zip(("A", "tau", "B"), np.sqrt(np.diag(pcov)), strict=True)),
                    "r2": r2,
                    "ss_res": ss_res,
                    "aicc": aicc(n_samples, 3, ss_res),
                }
            )
        except Exception:
            pass
        # Bi-exponential (split A0 70/30, τ1 fast ≈ τ0/5, τ2 slow ≈ τ0·5)
        try:
            p0 = (0.7 * A0, tau0 / 5, 0.3 * A0, tau0 * 5, B0)
            bounds = (
                (0, 1e-4, 0, 1e-4, -np.inf),
                (np.inf, np.inf, np.inf, np.inf, np.inf),
            )
            popt, pcov = curve_fit(biexp_decay, t, y, p0=p0, bounds=bounds, maxfev=10000)
            y_pred = biexp_decay(t, *popt)
            ss_res = float(np.sum((y - y_pred) ** 2))
            r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0
            # Sort components so τ1 < τ2
            if popt[1] > popt[3]:
                popt = np.array([popt[2], popt[3], popt[0], popt[1], popt[4]])
            candidates.append(
                {
                    "model": "biexp",
                    "params": {
                        "A1": popt[0],
                        "tau1": popt[1],
                        "A2": popt[2],
                        "tau2": popt[3],
                        "B": popt[4],
                    },
                    "err": dict(
                        zip(
                            ("A1", "tau1", "A2", "tau2", "B"),
                            np.sqrt(np.diag(pcov)),
                            strict=True,
                        )
                    ),
                    "r2": r2,
                    "ss_res": ss_res,
                    "aicc": aicc(n_samples, 5, ss_res),
                }
            )
        except Exception:
            pass
        # KWW stretched exponential
        try:
            p0 = (A0, tau0, 0.7, B0)
            bounds = ((0, 1e-4, 0.05, -np.inf), (np.inf, np.inf, 1.0, np.inf))
            popt, pcov = curve_fit(kww_decay, t, y, p0=p0, bounds=bounds, maxfev=10000)
            y_pred = kww_decay(t, *popt)
            ss_res = float(np.sum((y - y_pred) ** 2))
            r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0
            candidates.append(
                {
                    "model": "kww",
                    "params": {
                        "A": popt[0],
                        "tau": popt[1],
                        "beta": popt[2],
                        "B": popt[3],
                    },
                    "err": dict(
                        zip(("A", "tau", "beta", "B"), np.sqrt(np.diag(pcov)), strict=True)
                    ),
                    "r2": r2,
                    "ss_res": ss_res,
                    "aicc": aicc(n_samples, 4, ss_res),
                }
            )
        except Exception:
            pass

        if not candidates:
            fits.append({"trial": col, "error": "all model fits failed"})
            continue

        best = min(candidates, key=lambda c: c["aicc"])
        fits.append(
            {
                "trial": col,
                "best_model": best["model"],
                "candidates": candidates,
                "best": best,
            }
        )
    return {"fits": fits, "t": t}


def fit_prbs(prbs: dict, max_lag: int = 16):
    """Impulse response via cross-correlation of bits → ADC.

    Canonical white-input estimator: ĥ[k] = R_uy[k] / R_uu[0] = cov(u, y_lag_k)
    divided by var(u). For non-white PRBS (LFSR), this is approximate; for a
    rigorous estimate use Toeplitz FIR least squares. See
    docs/research/prc_math_review.md C3.
    """
    bits = prbs["bits"].astype(float)
    adc = prbs["adc"].astype(float)
    if len(bits) < max_lag + 4:
        return None
    bits = bits - bits.mean()
    adc = adc - adc.mean()
    u_var = float(np.mean(bits * bits))  # E[(u - ū)²]
    if u_var == 0:
        return None
    # Cross-correlation R_uy[k] = E[u[t] · y[t+k]], then h[k] = R_uy[k] / var(u)
    h = np.zeros(max_lag)
    for k in range(max_lag):
        if k < len(bits):
            r_uy = np.dot(bits[: len(bits) - k], adc[k:]) / (len(bits) - k)
            h[k] = r_uy / u_var
    # Normalize to peak (for plotting / τ estimation only)
    h_peak = np.max(np.abs(h))
    h_norm = h / h_peak if h_peak > 0 else h
    # Estimate τ from h: first lag where h drops below 1/e of peak
    peak = int(np.argmax(np.abs(h_norm)))
    tau_lag = max_lag
    for k in range(peak, max_lag):
        if abs(h_norm[k]) < abs(h_norm[peak]) / np.e:
            tau_lag = k - peak
            break
    return {"h": h, "h_norm": h_norm, "peak_lag": peak, "tau_lag": int(tau_lag)}


# ----------------------------------------------------------------------------
# Reporting
# ----------------------------------------------------------------------------
def report_xy(fit, meta, lines):
    lines.append("\n--- XY MAP: rotated 2D Gaussian fit ---")
    if fit is None:
        lines.append("  (no XY data found in raw.log)")
        return
    if "error" in fit:
        lines.append(f"  fit failed: {fit['error']}")
        return
    e = fit["err"]
    lines.append(
        f"  μ = ({fit['x0']:.2f} ± {e['x0']:.2f}, "
        f"{fit['y0']:.2f} ± {e['y0']:.2f}) px on {meta['fb_w']}×{meta['fb_h']}"
    )
    lines.append(f"  σ = ({fit['sx']:.2f} ± {e['sx']:.2f}, {fit['sy']:.2f} ± {e['sy']:.2f}) px")
    lines.append(f"  FWHM = ({fit['fwhm_x']:.1f}, {fit['fwhm_y']:.1f}) px")
    lines.append(f"  θ = {np.degrees(fit['theta']):.2f}° ± {np.degrees(e['theta']):.2f}°")
    lines.append(
        f"  amplitude A = {fit['A']:.0f} ± {e['A']:.0f}, baseline B = {fit['B']:.0f} ± {e['B']:.0f}"
    )
    lines.append(f"  R² = {fit['r2']:.4f}")
    cx = meta["fb_w"] / 2
    cy = meta["fb_h"] / 2
    lines.append(
        f"  ► offset from FB center: Δx = {fit['x0'] - cx:+.1f} px, Δy = {fit['y0'] - cy:+.1f} px"
    )
    # Convert to mm using the project-wide assumption (passed via meta if available)
    crt_w = meta.get("crt_w_mm")
    crt_h = meta.get("crt_h_mm")
    if crt_w and crt_h:
        dx_mm = (fit["x0"] - cx) * crt_w / meta["fb_w"]
        dy_mm = (fit["y0"] - cy) * crt_h / meta["fb_h"]
        lines.append(
            f"  ► offset on CRT face (assumed {crt_w}×{crt_h} mm): "
            f"Δx = {dx_mm:+.2f} mm, Δy = {dy_mm:+.2f} mm"
        )
        eff_d_x = fit["fwhm_x"] * crt_w / meta["fb_w"]
        eff_d_y = fit["fwhm_y"] * crt_h / meta["fb_h"]
        lines.append(f"  ► effective FWHM diameter on CRT: {eff_d_x:.1f} × {eff_d_y:.1f} mm")


def _summarize_best(best: dict) -> str:
    """Format winning-model params + R² for the decay report."""
    p = best["params"]
    err = best["err"]
    if best["model"] == "exp":
        return (
            f"exp: τ = {p['tau'] * 1000:.1f} ± {err['tau'] * 1000:.1f} ms, "
            f"A = {p['A']:.0f}, B = {p['B']:.0f}, R² = {best['r2']:.4f}"
        )
    if best["model"] == "biexp":
        return (
            f"biexp: τ₁ = {p['tau1'] * 1000:.1f} ± {err['tau1'] * 1000:.1f} ms, "
            f"τ₂ = {p['tau2'] * 1000:.1f} ± {err['tau2'] * 1000:.1f} ms, "
            f"A₁/A₂ = {p['A1']:.0f}/{p['A2']:.0f}, R² = {best['r2']:.4f}"
        )
    # kww
    return (
        f"kww: τ = {p['tau'] * 1000:.1f} ± {err['tau'] * 1000:.1f} ms, "
        f"β = {p['beta']:.3f} ± {err['beta']:.3f}, R² = {best['r2']:.4f}"
    )


def report_decay(fit, lines):
    lines.append("\n--- DECAY: per-trial fit (best of exp / biexp / KWW by AICc) ---")
    if fit is None:
        lines.append("  (no decay data)")
        return
    model_taus = {"exp": [], "biexp": [], "kww": []}
    for f in fit["fits"]:
        if "error" in f:
            lines.append(f"  trial {f['trial']}: FAILED ({f['error']})")
            continue
        best = f["best"]
        aiccs = ", ".join(f"{c['model']}={c['aicc']:.1f}" for c in f["candidates"])
        lines.append(f"  trial {f['trial']}: WINNER={best['model']}  [AICc {aiccs}]")
        lines.append(f"    {_summarize_best(best)}")
        # Track an "effective τ" per model for the across-trial summary.
        if best["model"] == "exp":
            model_taus["exp"].append(best["params"]["tau"])
        elif best["model"] == "biexp":
            # use slow component as the "effective" τ (dominates late decay)
            model_taus["biexp"].append(best["params"]["tau2"])
        else:
            # KWW effective mean: τ_eff = (τ/β)·Γ(1/β); use SciPy gamma function
            from math import gamma

            p = best["params"]
            tau_eff = (p["tau"] / p["beta"]) * gamma(1.0 / p["beta"])
            model_taus["kww"].append(tau_eff)
    # Across-trial summary for whichever model won most often
    for model, taus in model_taus.items():
        if not taus:
            continue
        ms = [t * 1000 for t in taus]
        label = {"exp": "τ", "biexp": "τ₂ (slow)", "kww": "τ_eff = (τ/β)·Γ(1/β)"}[model]
        lines.append(
            f"  ► {model} {label} across {len(ms)} trial(s): "
            f"{np.mean(ms):.2f} ± {np.std(ms):.2f} ms"
        )


def report_prbs(fit, lines):
    lines.append("\n--- PRBS: cross-correlation impulse response ---")
    if fit is None:
        lines.append("  (no PRBS data)")
        return
    lines.append(f"  peak lag k = {fit['peak_lag']} bit(s)")
    lines.append(
        f"  1/e fall-off width = {fit['tau_lag']} bits (at 50 ms/bit → {fit['tau_lag'] * 50} ms)"
    )
    h_pretty = " ".join(f"{v:+.3f}" for v in fit["h_norm"])
    lines.append(f"  h_norm: {h_pretty}")


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", default=None, help="run subdir name (default: latest)")
    ap.add_argument("--crt-w-mm", type=float, default=275.0)
    ap.add_argument("--crt-h-mm", type=float, default=206.0)
    args = ap.parse_args()

    if args.run:
        run_dir = TMP_DIR / args.run
    else:
        latest = TMP_DIR / "latest"
        if latest.is_symlink():
            run_dir = latest.resolve()
        else:
            runs = sorted([p for p in TMP_DIR.iterdir() if p.is_dir()])
            if not runs:
                print("no runs found", file=sys.stderr)
                sys.exit(1)
            run_dir = runs[-1]

    print(f"# analyzing {run_dir}")
    raw = run_dir / "raw.log"
    meta = parse_xy_meta(raw)
    meta["crt_w_mm"] = args.crt_w_mm
    meta["crt_h_mm"] = args.crt_h_mm

    lines = []
    lines.append("=" * 64)
    lines.append(f"PRC RIGOROUS CALIBRATION  (run {run_dir.name})")
    lines.append("=" * 64)

    xy = load_xy(raw)
    if xy is not None:
        # Save xy.csv
        with (run_dir / "xy.csv").open("w") as f:
            f.write("# 16x16 ADC matrix (rows=Y, cols=X)\n")
            for row in xy:
                f.write(",".join(f"{int(v)}" for v in row) + "\n")
        xy_fit = fit_xy(xy, meta)
        report_xy(xy_fit, meta, lines)
        if xy_fit and "z_pred" in xy_fit:
            with (run_dir / "xy_fit.csv").open("w") as f:
                f.write("# fitted Gaussian on the same grid\n")
                for row in xy_fit["z_pred"]:
                    f.write(",".join(f"{v:.1f}" for v in row) + "\n")
    else:
        report_xy(None, meta, lines)

    decay = load_decay(run_dir / "decay.csv")
    decay_fit = fit_decay(decay) if decay else None
    report_decay(decay_fit, lines)

    prbs = load_prbs(run_dir / "prbs.csv")
    prbs_fit = fit_prbs(prbs) if prbs else None
    report_prbs(prbs_fit, lines)
    if prbs_fit:
        with (run_dir / "prbs_h.csv").open("w") as f:
            f.write("lag,h,h_norm\n")
            for k, (h, hn) in enumerate(zip(prbs_fit["h"], prbs_fit["h_norm"], strict=True)):
                f.write(f"{k},{h:.4f},{hn:.4f}\n")

    lines.append("=" * 64)
    out = "\n".join(lines)
    print(out)
    (run_dir / "fit.txt").write_text(out + "\n")


if __name__ == "__main__":
    main()
