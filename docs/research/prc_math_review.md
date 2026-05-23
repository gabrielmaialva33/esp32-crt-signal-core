# PRC Pipeline — Math & Numerics Review (2026-05-23)

Two-model audit of formulas, fit procedures, and timing math in the
Physical Reservoir Computing pipeline.

- **Claude Opus 4.7** review (cached): `/tmp/claude_math_review.md`
- **Codex (GPT-5.5 high)** review (cached): `/tmp/codex_math_review.md`

Sources cited where the reviewers diverge are flagged with `[C]` / `[X]`.

---

## 0. CRITICAL FINDING (2026-05-23 deeper-precision audit)

**The "phosphor decay" model assumption is wrong.** Driving the system with a
sharper precision pipeline (firmware oversampling kSampleN=64, GPU bootstrap
of three exponential variants, full-residual Durbin-Watson diagnostic)
revealed:

* Single-exponential R² = 0.86, **Durbin-Watson = 0.04** (≈ 2 means residuals
  are i.i.d.; 0.04 means residuals are **structured**, i.e. model mismatch).
* Inspection of decay.csv showed the system stays at LIGHT level (≈3000 LSB)
  for the first ~300 ms after FB-off, then drops sharply to DARK level over
  ~100 ms.
* This is **sigmoidal**, not exponential. Fitting a logistic
  `y(t) = A / (1 + exp((t − t₀)/k)) + B` gives R² = 0.9963, DW = 1.20,
  ΔAICc > 2000 vs exp.
* The "τ ≈ 313–988 ms" numbers reported in earlier runs were single-exp fits
  of a sigmoidal response — physically meaningless.

**Best-fit logistic parameters (gpu_bootstrap, 3 trials, 5000 reps each):**

| Param | Mean across trials | 95% CI per trial |
|-------|--------------------|------------------|
| t₀    | 440.79 ± 3.76 ms   | ±0.6 ms (19× tighter than exp τ) |
| k     | 31.20 ms           | ±0.5 ms |
| A     | 2649               | ±15 |
| B     | 365                | ±5 |

**Physical interpretation:** the "decay" is dominated by the IR-LED
reverse-bias charge integration time, not CRT phosphor decay (which is
10–50 ms for typical P22 and should be hidden under the sensor's slower
response). The 10–90 % transition width is ≈ 4.39·k ≈ 137 ms.

**Action items:**

* Logistic is now part of the AICc model menu in
  `tools/prc/analyze.py:79` and `tools/prc/gpu_bootstrap.py`. Future fits
  pick whichever model has the lowest AICc.
* If we want to recover the true phosphor τ, we need a **faster sensor**:
  either a discrete photodiode with low capacitance, or a dedicated TIA
  front-end, or a fast-shutter optical pickup. The IR-LED-as-photodiode
  trick has a ~440 ms blind spot.
* PRC fading-memory measurements are therefore tracking the **sensor's**
  memory, not the phosphor's. Memory capacity at kSettleMs = 350 ms gives
  MC = 5.9/8 because the sensor saturates at ~440 ms ≫ tick — fading
  memory ≈ 7 ticks ≈ 2.45 s, which IS the sensor's relaxation behavior.

---

## 1. Critical issues (fix first — affect reported numbers)

### C1. `mc_at_delay` uses R², not Jaeger's squared Pearson correlation
**File:** `tools/prc/memory_capacity.py:106-109`
**Both reviewers flagged it.**

Jaeger (2002) defines memory capacity at delay k as

    MC_k = cov²(u[t-k], û_k(t)) / (var(u) · var(û_k))

i.e. the **squared Pearson correlation** between the target and the readout
output. For an unbiased OLS predictor `R² = corr²`; for ridge with `λ > 0` the
predictor is biased and `R² < corr²` in general. The current code returns
`max(R², 0)` and silently zeros negative R² — that masks fit failures and
contributes spurious "memory" to the total MC.

**Fix:**
```python
y_pred = Xv_c @ w + mu_y
corr = np.corrcoef(yv2, y_pred)[0, 1]
mc_k = float(corr ** 2) if np.isfinite(corr) else 0.0
return mc_k, float(yv.var())
```

Drop the `max(..., 0)` clamp; if you want to keep it, expose it as a flag and
log when it fires.

Reference: Jaeger H. (2002) "Short term memory in echo state networks,"
GMD Report 152, eqs. (2)–(4).

### C2. `fit_prbs` impulse response missing `/var(u)` normalization
**File:** `tools/prc/analyze.py:256-267`
**Both reviewers flagged it.**

For an LTI system `y = h * u + n` with white input, the canonical estimator is

    ĥ[k] = E[(u[t]−ū)(y[t+k]−ȳ)] / E[(u[t]−ū)²] = R_uy[k] / R_uu[0]

The current code computes only `R_uy[k]`. For balanced PRBS `var(u) ≈ 0.25`
constant, so peak position and τ are unchanged, but the magnitudes in
`prbs_h.csv` are 4× the actual filter coefficients — interpretable only after
peak-normalization.

**Fix (1 line):**
```python
u_var = float(np.var(bits))   # bits already centered
if u_var > 0:
    h = h / u_var
```

Reference: NI System ID notes, "Cross-correlation analysis for impulse
response," `H = R_yu / R_uu`.

### C3. `fit_prbs` assumes white input but uses LFSR PRBS  `[X]`
**File:** `tools/prc/analyze.py:250-273`

Codex caught: finite-length PRBS isn't perfectly white. If `R_uu` isn't a
delta, `R_uy` equals `h * R_uu` and the cross-correlation estimator is
biased by the input autocorrelation.

Two fixes (pick one):

**(a) Toeplitz FIR least squares:**
```python
# Build U where U[t, k] = u[t-k]; solve min ||U w - y||² + λ||w||²
# Then w[k] ≈ h[k]
```

**(b) Frequency-domain deconvolution:**
```python
H = S_uy / (S_uu + eps)
h = ifft(H)
```

For our use (256 ticks, LFSR Galois period 2³²−1), the PRBS is *close enough*
to white that fix C2 alone gets ≥90% there. Fix C3 is a follow-on if we want
publication-quality numbers.

---

## 2. Important issues (bias or wrong physics — fix next)

### I1. Single-exponential model under-fits CRT phosphor + electronics
**File:** `tools/prc/analyze.py:54-55, 217-247`
**Both reviewers flagged it.**

Current model `A·exp(-t/τ) + B`. Three trials give `τ = 988–1212 ms` with
R²=0.84. The mediocre R² is the textbook signature of an under-parameterized
model; literature on phosphor decay uses bi-exponential or
**Kohlrausch–Williams–Watts (stretched exponential)**:

    bi-exp:  y(t) = A₁·exp(-t/τ₁) + A₂·exp(-t/τ₂) + B
    KWW:     y(t) = A·exp(-(t/τ)^β) + B,   0 < β ≤ 1

Add both models, fit all, pick by AICc (or BIC), and report parameters of
the winning model. Mean decay time for KWW comparable with single-exp τ:
`⟨τ⟩ = (τ/β)·Γ(1/β)`.

Reference: Phillips J.C. (1996), *Rep. Prog. Phys.* **59**, 1133–1208
(stretched-exponential phosphor decay).

### I2. `fit_decay` docstring says "joint fit" but loops per-trial  `[X]`
**File:** `tools/prc/analyze.py:217-247`

Docstring claims joint fit; the loop fits each trial independently. Two
options:

(a) Actually do joint fit with shared `τ`:
```python
def joint_exp(t, A1, A2, A3, B1, B2, B3, tau):
    ...
```

(b) Or just rename the docstring to "per-trial fit" and report
`τ_mean ± τ_std` across trials.

### I3. Decay test has an off-by-one + WHITE-leak into first DECAY sample  `[X]`
**File:** `main/app_main.c:792-797`

```c
for (int i = 0; i < kSamples; ++i) {
    adc_oneshot_read(...);            // read happens FIRST
    if (i == kSwitchAt) {
        app_fb_fill(&s_fb, 0);        // then FB cleared
    }
    esp_rom_delay_us(kPeriodUs);
}
```

Sample `i = kSwitchAt` is logged as the first DECAY sample but was measured
while the screen was still WHITE. This inflates `A₀` and pulls τ longer.

**Fix in firmware (preferred):**
```c
for (int i = 0; i < kSamples; ++i) {
    if (i == kSwitchAt) {
        app_fb_fill(&s_fb, 0);
        esp_rom_delay_us(50);    // small settle so first sample is DECAY
    }
    adc_oneshot_read(...);
    esp_rom_delay_us(kPeriodUs);
}
```

**Alternative in analyzer:** treat `i > kSwitchAt` (strict) as DECAY and
re-zero the time axis at `(i − kSwitchAt − 1) · period_us`.

### I4. Decay pre-phase only 320 ms; phosphor τ ≈ 1.2 s → not at steady state
**File:** `main/app_main.c:769-771`
**Both reviewers flagged it.**

With pre-phase 320 ms and fitted τ=1212 ms, the system reaches only
`1−exp(−0.32/1.2) = 23%` of asymptotic steady-state. `A` and `B` from the fit
are biased; only τ is preserved (since the fit only sees post-switch samples).

**Fix:** extend `kSamples` from 256 to ≥ 1024 and put the switch at 1/4 of
the way (so pre-phase ≥ 1.28 s). Total burst window grows from 1.28 s to
≥ 5 s. Memory cost: 4 KB. ADC bandwidth is fine at 200 Hz.

### I5. `kSettleMs = 350 ms` in gprbs erases the dynamics MC tries to measure  `[X]`
**File:** `main/app_main.c:693`

Comment says "> 250ms phosphor tau → discriminated levels". That gives a
clean per-level static curve, but **memory capacity measures fading memory of
past inputs in the current state**. If each tick waits for the system to
nearly settle, fading memory shrinks by construction.

**Fix:** sweep `kSettleMs ∈ {5, 10, 20, 50, 100, 350}` and plot MC vs settle.
The interesting operating point for PRC is `kSettleMs ≈ τ/3` to `τ/10`.

### I6. Ridge regression has no feature standardization
**File:** `tools/prc/memory_capacity.py:96-104`
**Both reviewers flagged it.**

Ridge penalty `λ·||w||²` is scale-dependent. ADC variance differs across
delays (older taps have more phosphor decay). Without standardization, later
features are over-penalized.

**Fix:**
```python
mu_X = Xt.mean(axis=0)
sigma_X = Xt.std(axis=0)
sigma_X[sigma_X == 0] = 1.0
Xt_c = (Xt - mu_X) / sigma_X
Xv_c = (Xv2 - mu_X) / sigma_X
```

Reference: scikit-learn Ridge docs — penalty units depend on feature scale.

### I7. λ hardcoded; no CV
**File:** `tools/prc/memory_capacity.py:119`

`--ridge default=1e-3` is arbitrary. For a benchmark either:
- sweep `λ ∈ logspace(-6, 2, 9)` with blocked CV and pick max-MC λ
- or use OLS (`λ = 0`) and a pseudo-inverse — Jaeger's original definition

### I8. Multi-input MC bound `4·W` is wrong  `[X]`
**File:** `tools/prc/memory_capacity.py:204-208`

The state vector `X` has dimension W. Four parallel tasks on the same state
share the same readout dimensionality. Per Jaeger, `Σ_k MC_k ≤ rank(X) ≤ W`,
not `4·W`. Currently the code reports `4·W` as the "theoretical upper bound,"
which can be exceeded by the four per-quadrant sums.

**Fix:**
```python
upper_bound = np.linalg.matrix_rank(X)   # typically == W for non-degenerate
```

### I9. `k=0` in MC totals inflates "memory"  `[X]`
**File:** `tools/prc/memory_capacity.py:154, 193, 235`

Jaeger MC measures recovery of past inputs `u[t−k]` for `k ≥ 1`. Including
`k = 0` measures feedthrough (input visible in current state) which always
gives high R². Total MC including k=0 is artificially inflated.

**Fix:** default loop `for k in range(1, max_delay + 1)` and add an
`--include-zero` flag if needed.

### I10. `curve_fit` ±1σ is optimistic (residuals not iid)  `[X]`
**File:** `tools/prc/analyze.py:183-186, 236-237`

`scipy.optimize.curve_fit` reports `pcov` assuming residuals are i.i.d.
Gaussian with homoscedastic variance. ADC noise from CRT scan + jitter is
heteroscedastic and autocorrelated. Reported `± 1σ` is therefore optimistic.

**Recommendation:** either bootstrap by trial (`n_trials = 3`, resample
samples within each trial) or label the report "fit covariance under iid
residual assumption." Bootstrap snippet:
```python
B = 200
taus = []
for _ in range(B):
    idx = np.random.choice(n, n, replace=True)
    popt, _ = curve_fit(model, t[idx], y[idx])
    taus.append(popt[1])
tau_ci = np.percentile(taus, [2.5, 97.5])
```

### I11. Calibration loses sign of swing  `[X]`
**File:** `main/app_main.c:510-514, 887-895`

`swing = light_mean − dark_mean` is stored as magnitude only. If LED
polarity inverts (reverse-biased photodiode in different orientation),
swing < 0 and the detector never triggers LIGHT.

**Fix:**
```c
const int swing = light_mean - dark_mean;
const int abs_swing = (swing < 0) ? -swing : swing;
s_ir_polarity = (swing >= 0) ? +1 : -1;
// detection:
delta = polarity * (v - baseline);
if (delta > threshold) ...
```

---

## 3. Minor issues / nits

### M1. FWHM uses `2.355` (4 sig figs) instead of exact constant  `[both]`
Trivial fix:
```python
FWHM_FACTOR = 2.0 * np.sqrt(2.0 * np.log(2.0))   # 2.354820045...
```

### M2. Rotation `θ ∈ (-π, π)` has redundant degeneracy
Restrict to `(-π/2, π/2)`. Rotated Gaussian has period π.

### M3. Decay `tau0 = (t[-1]-t[0])/5` initial guess fragile when τ ≈ window
Use a smarter `p0` (e.g., fit `log(y - y_min + 1)` linearly to get an initial
slope, then refine).

### M4. Sliding window mixes "physical memory" with "digital tapped delay line"  `[X]`
**File:** `tools/prc/memory_capacity.py:68-74`

`X[t] = [adc[t], adc[t-1], ...]` adds a software FIR delay line on top of
the physical reservoir. Report `W = 1` (scalar) as a baseline for the
**physical-only** memory; current `W > 1` benchmarks reservoir + delay line.

### M5. Ellipse arc rounding biased for negative coords  `[X]`
**File:** `main/app_main.c:341-342, 349-350`

`(int)(rx*cos(a) + 0.5f)` truncates toward zero for negative values; use
`lroundf()` for round-half-to-even consistent across signs.

### M6. PRBS reporter time scale hardcoded to 50 ms/bit  `[X]`
**File:** `tools/prc/analyze.py:347-348`

Firmware now runs at 350 ms/bit (post 2026-05-23 fix). The analyzer prints
seconds derived from a stale `50 ms` constant. Either parse `kSettleMs` from
the log (recommended) or accept a CLI flag.

### M7. Comment errors in firmware
**File:** `main/app_main.c:762, 770-771`
**Codex flagged.**

- Says "10 kHz" but `kPeriodUs = 5000` → 200 Hz
- Says "1.28 s post-switch window" but post = 960 ms; total = 1.28 s

Update comments to match code (or change the code to match the intent).

### M8. CRT overscan ignored in mm↔pixel mapping
`APP_CRT_VISIBLE_W_MM = 275` assumes FB fills the entire visible area, but
CRTs typically have 5–10% overscan/underscan. Absolute mm values are off by
that much; shape (ellipse aspect) is preserved. Calibrate `x_mm = a_x·x +
b_x` empirically if absolute mm matters.

### M9. LFSR `0xA3000000` is verified maximum-length (period 2³²−1)  `[X]`
Codex verified primitivity of `x³² + x³⁰ + x²⁶ + x²⁵ + 1` by checking
`x^N = 1` and `x^(N/q) ≠ 1` for `q ∈ {3, 5, 17, 257, 65537}` (the prime
factors of 2³²−1). I empirically verified period > 5×10⁷.

If the doc/comment claims **31-bit**, it's wrong — this is a **32-bit Galois
LFSR**. Either fix the comment or, for an actual 31-bit max-length LFSR,
switch to `M = 0x48000000` (taps 31, 28) with `seed & 0x7FFFFFFF`.

---

## 4. Verified correct (no action)

- Rotated 2D Gaussian formula (analyze.py:43-51).
- Tapped delay line construction in `build_state_matrix`.
- mm↔pixel ellipse derivation (the per-axis scaling correctly handles
  non-square FB pixels on the CRT face).
- `app_ir_capture_window` ADC averaging.
- I2S word-swap, blanking/sync timing (out of scope here; signal-side math
  is standardized per BT.470).

---

## 5. Priority-ordered action list

| # | Severity | File:line | Fix effort |
|---|----------|-----------|-----------|
| 1 | CRITICAL | memory_capacity.py:106-109 | switch R² → corr², drop max() clamp |
| 2 | CRITICAL | analyze.py:256-265 | add `/var(u)` to cross-corr |
| 3 | IMPORTANT | analyze.py:54-55 | add KWW + bi-exp models, pick by AICc |
| 4 | IMPORTANT | memory_capacity.py:96-104 | standardize features |
| 5 | IMPORTANT | memory_capacity.py:204 | replace `4·W` with `rank(X)` |
| 6 | IMPORTANT | memory_capacity.py:154 | start k=1, not k=0 |
| 7 | IMPORTANT | app_main.c:792-797 | fix off-by-one (clear FB before sample) |
| 8 | IMPORTANT | app_main.c:769-771 | extend `kSamples` to ≥1024 (pre ≥ 4·τ) |
| 9 | IMPORTANT | app_main.c:693 | sweep `kSettleMs` for MC (not just 350ms) |
| 10 | IMPORTANT | app_main.c:510-514 | store polarity, use signed delta |
| 11 | IMPORTANT | analyze.py:217-247 | rename "joint" → "per-trial" OR do joint |
| 12 | MINOR | analyze.py | FWHM exact const, θ bound, fit_decay p0, time scale |
| 13 | MINOR | app_main.c | comment 10 kHz → 200 Hz, 1.28 s → 960 ms post |
| 14 | NIT | app_main.c LFSR | doc says 31-bit but it's 32-bit max-length |

---

## 6. References used

1. Jaeger H. (2002), *Short term memory in echo state networks*,
   GMD Report 152 — https://www.ai.rug.nl/minds/uploads/STMEchoStatesTechRep.pdf
2. Wikipedia: *Gaussian function § 2-D* —
   https://en.wikipedia.org/wiki/Gaussian_function#Two-dimensional_Gaussian_function
3. Phillips J.C. (1996), *Stretched exponential relaxation in disordered
   systems*, Rep. Prog. Phys. **59**, 1133.
4. Williams G., Watts D.C. (1970), *Non-symmetrical dielectric relaxation
   from a simple empirical decay function*, Trans. Faraday Soc. **66**, 80.
5. Alfke P. (1996), *Efficient Shift Registers, LFSR Counters, and Long
   Pseudo-Random Sequence Generators*, Xilinx XAPP 052.
6. scikit-learn Ridge docs — penalty/scale dependence.
7. PRC review citing Jaeger MC —
   https://link.springer.com/article/10.1007/s11047-024-09997-y
