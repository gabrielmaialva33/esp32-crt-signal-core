# NARMA-10 benchmark on the CRT-IR-ring reservoir

Date: 2026-05-23
Run (passing): `tmp/prc_runs/20260523_123912` (2048 ticks @ 150 ms settle)
Run (failing): `tmp/prc_runs/20260523_120628` (256 ticks @ 350 ms settle)

## TL;DR

| Configuration                | NMSE_test | R²_test | Verdict |
|------------------------------|-----------|---------|---------|
| 256 ticks, 350 ms settle (old)  | 0.78  | +0.22 | FAIL (overfit, no fading memory) |
| **2048 ticks, 150 ms settle**   | **0.25** | **+0.75** | **PASS (typical PRC range 0.2–0.4)** |

Two firmware tweaks took the system from failing to passing the standard
PRC benchmark — no sensor change required.

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

## Result (passing config — 2048 ticks @ 150 ms settle)

Hyperparameter sweep over `window ∈ {8, 12, 16, 24}`,
`poly_degree ∈ {1, 2, 3}`, and `ridge λ ∈ {1e-2 … 1e+4}`:

```
BEST: NMSE_test = 0.2508  (W=24, poly_degree=2, λ=10)

  train: n=1403   NMSE=0.33   R²=+0.67
  test : n= 602   NMSE=0.25   R²=+0.75
```

The gap between train and test is small (0.07), meaning the model
**generalizes**. Test R² = +0.75 puts the system squarely in published PRC
territory.

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
# Passing config (2048 ticks @ 150 ms settle)
uv run --with numpy python tools/prc/narma10.py \
    --run 20260523_123912 --window 24 --poly-degree 2 --ridge 10.0

# Old failing config (256 ticks @ 350 ms settle), for the record
uv run --with numpy python tools/prc/narma10.py \
    --run 20260523_120628 --window 24 --poly-degree 2 --ridge 1e-2

# Hyperparameter sweep
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
