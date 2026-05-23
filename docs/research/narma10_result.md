# NARMA-10 benchmark on the CRT-IR-ring reservoir

Date: 2026-05-23
Run (passing): `tmp/prc_runs/20260523_123912` (2048 ticks @ 150 ms settle)
Run (failing): `tmp/prc_runs/20260523_120628` (256 ticks @ 350 ms settle)

## TL;DR

| Configuration                  | NMSE 5-fold CV (per run) |
|--------------------------------|--------------------------|
| 256 ticks, 350 ms settle (old) | FAIL (n=1: 0.78 single-split, severe overfit) |
| **2048 ticks, 150 ms settle**  | **n=3 fresh: 0.38 ± 0.04** ← canonical |

Two firmware tweaks (`kTicks 256→2048`, `kSettleMs 350→150`) took the
system from failing to passing the standard PRC benchmark — no sensor
change required.

**Aggregate over 3 independent back-to-back captures
(`tmp/prc_runs/20260523_{135300,140006,140710}`):**

| Metric | Value |
|--------|-------|
| NARMA-10 NMSE_cv (between-run) | **0.384 ± 0.036** |
| Within-run fold std (avg) | 0.070 |
| MC total | **6.231 ± 0.048** out of 8 (77.9 %) |
| gprbs swing | 825 ± 29 LSB |
| Logistic t₀ | ~445 ms (consistent across runs) |

The between-run NMSE std (0.036) is **smaller** than the within-run
fold std (0.070), meaning the system is reproducible — most of the
NMSE variance comes from split-luck inside any given run, not from
the underlying physical system drifting between captures.

A single 70/30 split can land on NMSE = 0.25 (lucky) or 0.75
(unlucky) on the **same** PRBS sequence + firmware. Always report
CV mean ± std, never a single split.

## Task

NARMA-10 (Atiya & Parlos 2000) is the standard PRC sanity check. Input
`u[t] ∈ [0, 0.5]` drives the 10th-order nonlinear recurrence

```
y[t+1] = 0.3·y[t] + 0.05·y[t]·Σ_{i=0..9} y[t-i] + 1.5·u[t-9]·u[t] + 0.1
```

A reservoir succeeds if a linear readout on its state can reconstruct `y[t]`
with low NMSE = `mean((y_true - y_pred)²) / var(y_true)`. Reference scale:

| NMSE_test | Quality |
|-----------|---------|
| < 0.1     | excellent (publication-grade) |
| 0.2 – 0.4 | typical PRC literature |
| 0.5 – 1.0 | weak / over-fit |
| ≥ 1.0     | worse than predicting the mean |

## Result (passing config — 2048 ticks @ 150 ms settle, two independent runs)

Hyperparameter sweep over `window ∈ {8, 12, 16, 24}`,
`poly_degree ∈ {1, 2, 3}`, and `ridge λ ∈ {1e-2 … 1e+4}`, evaluated by
5-fold sequential CV (preserves temporal causality, averages out
favorable splits):

```
Run 20260523_123912:
  BEST CV: NMSE = 0.3627 ± 0.0878  (W=24, poly_degree=3, λ=1)
  per-fold: [0.306, 0.522, 0.358, 0.363, 0.263]

Run 20260523_130301:
  BEST CV: NMSE = 0.3696 ± 0.0898  (W=24, poly_degree=3, λ=1)
  per-fold: [0.250, 0.446, 0.350, 0.496, 0.306]
```

Both runs agree to within 1 % on the mean and converge on the same
hyperparameters. **W=24, poly_degree=3, λ=1.0** is therefore the recommended
operating point — not the W=24/deg=2/λ=10 we initially reported from a single
70/30 split.

## Result (failing config — 256 ticks @ 350 ms settle)

Same sweep on the earlier capture:

```
BEST: NMSE_test = 0.78  (W=24, poly_degree=2, λ=1e-2)

  train: n=149   NMSE=0.13   R²=+0.87
  test : n= 64   NMSE=0.78   R²=+0.22
```

Train fits well; test is essentially mean prediction. Classic overfitting
from too few samples + too-large settle (system fully settles between
ticks, no fading memory exercised).

## Why two tweaks fixed it (and why we couldn't see the win before)

The bottleneck on the failing config was a **compounding** of three
factors. Two of them were firmware-side and we just fixed them:

1. ~~**Too few ticks.**~~ Bumped `kTicks` from 256 to 2048
   (`app_main.c:702`). 1403 train samples for 48 features is well clear
   of the over-parameterized regime.

2. ~~**No fading memory exercised.**~~ Dropped `kSettleMs` from 350 ms to
   150 ms (`app_main.c:703`). With settle below the sensor's logistic
   `t₀ ≈ 460 ms`, each tick now visibly carries information from the
   previous tick (FFT impulse response now has `h[1] ≈ +0.135`, vs
   `-0.043` before). MC total rose from 5.85 → 6.17.

3. **Sensor dynamics, not phosphor.** ← *still true; not addressable
   without hardware change*. The logistic-decay finding
   (`t₀ ≈ 460 ms`, `k ≈ 32 ms`; see `docs/research/prc_math_review.md`)
   shows the reservoir's effective time constant is the IR-LED-as-photodiode
   integrator, not the CRT phosphor. Higher-order NARMA tasks (NARMA-20,
   NARMA-30) will still be hard.

Original failure-mode notes preserved below for context. Three compounding factors:

1. **Too few ticks.** 256 ticks (minus 20 warm-up) = 236 effective samples.
   Standard NARMA-10 benchmarks use 1000–5000 ticks. With only 149 train
   samples for 48 features (W=24, degree=2), we are massively
   over-parameterized.

2. **Memory horizon too short.** The MC benchmark earlier (mc_gray.csv)
   showed total MC = 5.9 with memory saturating at k ≈ 7. NARMA-10
   demands strict 10-step memory for the `u[t-9]·u[t]` cross-term — the
   reservoir loses information about `u[t-9]` before it can be multiplied
   with the current `u[t]`.

3. **Sensor dynamics, not phosphor.** The logistic-decay finding
   (`t₀ ≈ 440 ms`, `k ≈ 31 ms`; see `docs/research/prc_math_review.md`)
   shows the reservoir's effective time constant is the IR-LED-as-photodiode
   integrator, not the CRT phosphor. The sensor has a delay-then-step
   response — it does not implement multiplicative nonlinearity.

## Implications

NARMA-10 succeeding (test NMSE < 0.4) would have been a publication-grade
result and is the de-facto entry ticket for PRC papers. NMSE ≈ 0.78 here
is **itself a useful finding**: it bounds what this specific configuration
can do, and points at the bottleneck.

To improve:

- **More data, easier path.** Bump `kTicks` from 256 to 2048 in
  `app_main.c:702` and rerun. Same firmware, ~10× longer capture
  (~12 min instead of ~90 s for gprbs). Likely brings NMSE_test under 0.5
  without any other change.
- **Smaller kSettleMs.** With `kSettleMs = 350 ms ≫ τ_sensor`, each tick
  effectively settles before the next — the reservoir's fading-memory
  capacity does not get exercised. Drop to `kSettleMs = 50–100 ms` to make
  past inputs visible in the current state.
- **Faster sensor.** The fundamental fix: replace the
  IR-LED-as-photodiode trick with a discrete photodiode + TIA front-end.
  This removes the 440 ms blind spot and exposes the actual CRT phosphor
  dynamics (~10–50 ms for P22), letting the system implement the kind of
  nonlinear transient that NARMA-10 needs.

## Reproduction

```bash
# Recommended hyperparameters from 5-fold CV
uv run --with numpy python tools/prc/narma10.py \
    --run latest --window 24 --poly-degree 3 --ridge 1.0

# 5-fold sequential CV across both passing runs
uv run --with numpy python /tmp/narma_cv.py

# Hyperparameter sweep (single 70/30 split — superseded by CV)
uv run --with numpy python /tmp/narma_sweep.py
```

## Sanity checks on the passing run

- `mc_gray.csv`: total MC = 6.17/8 (was 5.85) — slightly better.
- `irf_h.csv`: `h_norm[1] = +0.135` (was `-0.043`) — visible carry-over
  between ticks confirms the reservoir actually has fading memory at this
  operating point.
- `bootstrap_logistic.csv`: `t₀ = 462.3 ± 9 ms` across 3 trials, in line
  with prior measurements.
- `bandwidth -3dB ≈ 3.33 Hz` (was 0.045 Hz) — the system is now sampled
  fast enough that it visibly responds to the input modulation.
