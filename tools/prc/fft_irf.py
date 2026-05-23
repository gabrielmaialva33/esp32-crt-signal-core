#!/usr/bin/env python3
"""Spectral impulse-response estimation for the PRC pipeline.

Estimates the system frequency response H(f) and impulse response h[k] via
spectral deconvolution. Handles arbitrary input (no white-input assumption):

    H(f) = S_uy(f) / (S_uu(f) + ε)
    h[k] = IFFT( H(f) )

where S_uu, S_uy are the power and cross spectra computed via Welch's method
with a Hann window. ε is a Tikhonov regularizer to avoid division by zero in
bands where the input has no power.

This is the rigorous alternative to the time-domain cross-correlation in
analyze.py:fit_prbs(), which assumes the input is approximately white. For
finite-length LFSR PRBS the autocorrelation is *close to* but not exactly a
delta; FFT-based deconvolution does the right thing either way.

Usage:
    python tools/prc/fft_irf.py --run 20260523_114729
    python tools/prc/fft_irf.py --run latest --epsilon 1e-3 --nperseg 64
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np
from scipy.signal import csd, welch

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
TMP_DIR = PROJECT_ROOT / "tmp" / "prc_runs"


def load_gprbs(path: Path) -> tuple[np.ndarray, np.ndarray]:
    """Read gprbs.csv → (input levels, ADC samples) as 1-D float arrays."""
    lvls, adcs = [], []
    with path.open() as f:
        for line in f:
            if line.startswith("#") or line.startswith("tick"):
                continue
            parts = line.strip().split(",")
            if len(parts) >= 4:
                lvls.append(int(parts[1]))
                adcs.append(int(parts[3]))
    return np.array(lvls, dtype=float), np.array(adcs, dtype=float)


def parse_tick_ms(run_dir: Path, default: float = 350.0) -> float:
    """Best-effort: pull kSettleMs from raw.log; falls back to default."""
    log = run_dir / "raw.log"
    if not log.exists():
        return default
    text = log.read_text(errors="replace")
    # Firmware doesn't log kSettleMs directly today; we leave a hook here for
    # when it does. For now we just return the default.
    _ = text
    return default


def fft_irf(
    u: np.ndarray,
    y: np.ndarray,
    fs: float,
    nperseg: int = 64,
    epsilon: float = 1e-3,
    n_lag: int = 32,
) -> dict:
    """Spectral impulse-response estimate.

    Args:
        u, y: 1-D input / output sequences (same length, same sampling rate).
        fs: sampling frequency in Hz (1 / bit_duration).
        nperseg: Welch window length (samples). Trade-off: bigger = better
            frequency resolution, fewer averages, noisier H(f).
        epsilon: Tikhonov regularizer on |S_uu|. Prevents division by zero in
            bands the input doesn't excite. Relative to max(|S_uu|).
        n_lag: how many time-domain h[k] taps to return.

    Returns:
        dict with keys: freq, S_uu, S_uy, H_mag, H_phase, h, h_norm,
        peak_lag, tau_lag, fs, nperseg, epsilon.
    """
    u = np.asarray(u, dtype=float)
    y = np.asarray(y, dtype=float)
    u = u - u.mean()
    y = y - y.mean()
    if len(u) != len(y):
        raise ValueError("u and y must have the same length")
    if len(u) < nperseg * 2:
        # Fall back to a smaller window if data is short.
        nperseg = max(8, len(u) // 4)

    # Welch power / cross spectra (one-sided, real signals).
    f_uu, S_uu = welch(u, fs=fs, nperseg=nperseg, return_onesided=True)
    f_uy, S_uy = csd(u, y, fs=fs, nperseg=nperseg, return_onesided=True)
    assert np.allclose(f_uu, f_uy)

    # Regularize denominator so bands with no input power don't blow up H.
    S_uu_max = float(np.max(np.abs(S_uu)))
    reg = epsilon * S_uu_max
    H = S_uy / (S_uu + reg)

    # Reconstruct time-domain impulse response via IFFT. Take only the first
    # n_lag taps (the rest is the same energy mirrored due to symmetry).
    # For one-sided spectrum we need to assemble the full-band H first.
    n_full = (len(H) - 1) * 2  # FFT length implied by one-sided result
    H_full = np.zeros(n_full, dtype=complex)
    H_full[: len(H)] = H
    H_full[-(len(H) - 1) :] = np.conj(H[1:][::-1])
    h_full = np.fft.ifft(H_full).real
    h = h_full[:n_lag]

    # Normalize for plotting / τ estimation
    h_peak_idx = int(np.argmax(np.abs(h)))
    h_peak = float(np.abs(h[h_peak_idx]))
    h_norm = h / h_peak if h_peak > 0 else h.copy()

    # Estimate τ as the first lag past peak where h falls below 1/e of peak.
    tau_lag = n_lag
    for k in range(h_peak_idx, n_lag):
        if abs(h_norm[k]) < abs(h_norm[h_peak_idx]) / np.e:
            tau_lag = k - h_peak_idx
            break

    return {
        "freq": f_uu,
        "S_uu": S_uu,
        "S_uy": S_uy,
        "H_mag": np.abs(H),
        "H_phase": np.angle(H, deg=True),
        "h": h,
        "h_norm": h_norm,
        "peak_lag": h_peak_idx,
        "tau_lag": tau_lag,
        "fs": fs,
        "nperseg": nperseg,
        "epsilon": epsilon,
    }


def write_irf_csv(result: dict, run_dir: Path, fs: float) -> None:
    """Persist h[k] and H(f) to CSVs for downstream plotting."""
    h_path = run_dir / "irf_h.csv"
    with h_path.open("w") as f:
        w = csv.writer(f)
        w.writerow(["lag_idx", "time_s", "h", "h_norm"])
        for k, (h, hn) in enumerate(
            zip(result["h"], result["h_norm"], strict=True),
        ):
            w.writerow([k, k / fs, f"{h:.6e}", f"{hn:.6f}"])

    H_path = run_dir / "irf_H.csv"
    with H_path.open("w") as f:
        w = csv.writer(f)
        w.writerow(["freq_hz", "S_uu", "S_uy_re", "S_uy_im", "H_mag", "H_phase_deg"])
        for fr, suu, suy, hm, hp in zip(
            result["freq"],
            result["S_uu"],
            result["S_uy"],
            result["H_mag"],
            result["H_phase"],
            strict=True,
        ):
            w.writerow(
                [
                    f"{fr:.6f}",
                    f"{suu:.6e}",
                    f"{suy.real:.6e}",
                    f"{suy.imag:.6e}",
                    f"{hm:.6e}",
                    f"{hp:.4f}",
                ]
            )


def report(result: dict, fs: float) -> str:
    h = result["h"]
    h_norm = result["h_norm"]
    tau_s = result["tau_lag"] / fs
    bw_3db_hz = float(result["freq"][np.argmin(np.abs(result["H_mag"] / result["H_mag"][0] - 0.5))])
    lines = [
        "--- FFT impulse response ---",
        f"  fs = {fs:.3f} Hz  (tick = {1000 / fs:.1f} ms)",
        f"  nperseg = {result['nperseg']}  epsilon = {result['epsilon']:.0e} · max(S_uu)",
        f"  peak lag = {result['peak_lag']} samples → t_peak = {result['peak_lag'] / fs * 1000:.1f} ms",
        f"  1/e fall-off width = {result['tau_lag']} samples → τ ≈ {tau_s * 1000:.1f} ms",
        f"  -3 dB bandwidth ≈ {bw_3db_hz:.3f} Hz",
        f"  |h|max = {np.max(np.abs(h)):.4e}",
        "  h_norm[0..7] = " + " ".join(f"{v:+.3f}" for v in h_norm[: min(8, len(h_norm))]),
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", default=None)
    ap.add_argument("--nperseg", type=int, default=64, help="Welch window length")
    ap.add_argument(
        "--epsilon",
        type=float,
        default=1e-3,
        help="regularizer on |S_uu|max to keep H well-defined where input has no energy",
    )
    ap.add_argument("--n-lag", type=int, default=32, help="time-domain taps to keep")
    ap.add_argument(
        "--tick-ms",
        type=float,
        default=None,
        help="bit duration in ms; auto-detected from raw.log when possible",
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
        print("no gprbs.csv in run; FFT IRF needs the gray-PRBS series", file=sys.stderr)
        sys.exit(2)

    u, y = load_gprbs(gprbs_path)
    tick_ms = args.tick_ms if args.tick_ms is not None else parse_tick_ms(run_dir)
    fs = 1000.0 / tick_ms

    print(f"# loaded {len(u)} ticks from {gprbs_path.relative_to(PROJECT_ROOT)}")
    print(f"# tick = {tick_ms:.1f} ms → fs = {fs:.3f} Hz")

    result = fft_irf(u, y, fs=fs, nperseg=args.nperseg, epsilon=args.epsilon, n_lag=args.n_lag)
    print(report(result, fs))

    write_irf_csv(result, run_dir, fs)
    print(f"# saved {(run_dir / 'irf_h.csv').relative_to(PROJECT_ROOT)}")
    print(f"# saved {(run_dir / 'irf_H.csv').relative_to(PROJECT_ROOT)}")


if __name__ == "__main__":
    main()
