#!/usr/bin/env python3
"""Generate tests/QuicCubicVectors.h — RFC 9438 CUBIC KAT vectors.

Machine-generated test material (same discipline as
tests/tls_certs/gen_tls_certs.py): no curve constant in the test file is
transcribed by hand. The generator

  1. mirrors the exact integer arithmetic of quic_congestion_cubic_window(),
     quic_congestion_cubic_time() and the congestion-avoidance branch of
     quic_congestion_on_ack() (src/quic/QuicCongestion.cpp, nginx-style
     byte-domain integer CUBIC),
  2. cross-checks every emitted curve value against the closed-form
     RFC 9438 4.1  W_cubic(t) = C*(t-K)^3 + W_max     (C = 0.4, byte domain:
     0.4 * (t/1000)^3 * mtu + w_max) and every K against
     RFC 9438 4.2  K = cbrt(W_max*(1-beta)/C)          (beta = 0.7),
  3. emits tests/QuicCubicVectors.h for QuicCongestionTest.

Run from the repository root:  python3 tests/quic_kat/gen_cubic_kat.py
"""

import math

BETA = 7  # kQuicCongestionCubicBeta (0.7)
C_INT = 4  # kQuicCongestionCubicC (0.4 via the 10^10 ms^3 scaling)
SIZE_MAX = 2**64 - 1

CC_SCALE = 10**10  # t is in ms; t^3 * mtu * C / 10^10 == C * (t/1000)^3 * mtu


def tdiv(a, b):
    """C++ int64 trunc division (Python // floors on negatives)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def cubic_cc(mtu):
    return tdiv(tdiv(CC_SCALE, mtu), C_INT)


def cubic_window(t_ms, w_max, mtu):
    """Mirrors quic_congestion_cubic_window() with idle == false."""
    if t_ms > 1000000:
        return SIZE_MAX
    if t_ms < -1000000:
        return 0
    cc = cubic_cc(mtu)
    w = tdiv(t_ms**3, cc) + w_max
    if w < 0:
        return 0
    return w  # int64 range: |t| <= 10^6 keeps w far below int64 max


def cubic_time(w_max, window, mtu):
    """Mirrors quic_congestion_cubic_time() (Newton iteration for cbrt)."""
    if w_max <= window:
        return 0
    cc = cubic_cc(mtu)
    v = (w_max - window) * cc
    x = 5000
    for _ in range(10):
        d = tdiv(tdiv(tdiv(v, x), x) - x, 3)
        x += d
        if abs(d) <= 100:
            break
    if x < 0:
        return 0
    return x


def initial_window(mtu):
    return min(10 * mtu, max(2 * mtu, 14720))


def enter_recovery(st, reduction_basis, now, path_mtu):
    """Mirrors the internal enter_recovery() reached via quic_congestion_on_loss()."""
    st["mtu"] = path_mtu
    st["recovery_start"] = now
    st["w_prior"] = st["window"]
    st["w_max"] = tdiv(st["window"] * (10 + BETA), 20) if st["window"] < st["w_max"] else st["window"]
    st["ssthresh"] = tdiv(reduction_basis * BETA, 10)
    st["window"] = max(st["ssthresh"], st["mtu"] * 2)
    st["w_est"] = st["window"]
    st["k"] = now + cubic_time(st["w_max"], st["window"], st["mtu"])
    st["idle_start"] = now


def canonical_loss_state():
    """The canonical post-loss state also used by LossUsesNginxMultiplicativeDecrease:
    init at t=0, 12000 bytes in flight, one 1200-byte packet lost at t=100."""
    st = {
        "in_flight": 0, "window": 0, "ssthresh": SIZE_MAX, "w_max": 0, "w_est": 0,
        "w_prior": 0, "mtu": 1200, "recovery_start": -1, "idle_start": 0, "k": 0,
        "idle": False,
    }
    st["window"] = initial_window(1200)
    st["in_flight"] = 12000
    # quic_congestion_on_loss(subtract 1200, basis = remaining in_flight = 10800)
    enter_recovery(st, 10800, 100, 1200)
    st["in_flight"] = 10800
    return st


def on_ack_avoid(st, packet_len, packet_number, send_time, now, oldest_sent_time):
    """Mirrors quic_congestion_on_ack() for the post-recovery, non-idle path.
    Returns the branch taken: 'est' | 'ramp' | 'cubic' | 'hold' (or an early-out)."""
    st["in_flight"] -= packet_len
    if now < st["recovery_start"]:
        st["recovery_start"] = oldest_sent_time - 1
    if send_time <= st["recovery_start"] or st["idle"]:
        return "recovery"
    if st["window"] < st["ssthresh"]:
        st["window"] += packet_len
        return "slow-start"
    w_cubic = cubic_window(now - st["k"], st["w_max"], st["mtu"])
    if st["window"] < st["w_prior"]:
        st["w_est"] += tdiv(tdiv(st["mtu"] * packet_len * 3 * (10 - BETA), (10 + BETA)), st["window"])
    else:
        st["w_est"] += tdiv(st["mtu"] * packet_len, st["window"])
    if w_cubic < st["w_est"]:
        st["window"] = st["w_est"]
        return "est"
    if w_cubic > st["window"]:
        if w_cubic >= tdiv(st["window"] * 3, 2):
            st["window"] += st["mtu"] // 2
            return "ramp"
        st["window"] += tdiv(st["mtu"] * (w_cubic - st["window"]), st["window"])
        return "cubic"
    return "hold"


# ---------------------------------------------------------------------------
# RFC 9438 closed-form cross-checks (step 2). Every emitted value must pass
# before the header is written.
# ---------------------------------------------------------------------------

def rfc_w_cubic(t_ms, w_max, mtu):
    """RFC 9438 4.1: C*(t-K)^3 + W_max with C = 0.4, W in bytes, t in ms."""
    return 0.4 * (t_ms / 1000.0) ** 3 * mtu + w_max


def cbrt(x):
    """math.cbrt needs 3.11; keep the generator runnable on older python3."""
    return math.copysign(abs(x) ** (1.0 / 3.0), x)


def rfc_k_ms(w_max, window, mtu):
    """RFC 9438 4.2: K = cbrt(W_max*(1-beta)/C), W in bytes, C = 0.4."""
    return cbrt((w_max - window) / (0.4 * mtu)) * 1000.0


def curve_tolerance(t_ms, mtu):
    """Bound for |integer model - RFC closed form| at identical t.

    cubic_cc() truncates twice, so cc_int >= cc_exact - 1.25; the growth term
    picks up |t^3| * 1.25 / cc^2 relative error, plus 1 from the final
    truncating division of the model itself.
    """
    cc = cubic_cc(mtu)
    growth = abs(tdiv(t_ms**3, cc))
    return 3.0 + growth * 1.25 / cc


def check_curve_point(name, t_ms, w_max, mtu, got):
    want = rfc_w_cubic(t_ms, w_max, mtu)
    tol = curve_tolerance(t_ms, mtu)
    if got == 0 or got == SIZE_MAX:
        # Clamp branches: verify the closed form is on the clamped side too.
        if got == 0 and want > 0.0:
            raise AssertionError(f"{name}: model clamped to 0 but RFC gives {want}")
        return
    if abs(got - want) > tol:
        raise AssertionError(f"{name}: t={t_ms} model={got} rfc={want:.3f} tol={tol:.3f}")


def check_k(name, w_max, window, mtu, got):
    want = rfc_k_ms(w_max, window, mtu)
    if abs(got - want) > K_TOLERANCE_MS:
        raise AssertionError(f"{name}: model K={got} rfc K={want:.3f} tol={K_TOLERANCE_MS}")


# Empirical bound for the converged domain, established by the sweep below:
# for w_max <= 60 MB the Newton fixpoint lands within ~8.7 ms of the closed
# form (the |d| <= 100 break leaves only a second-order residual). Beyond
# ~100 MB (K > ~40 s) the fixed 10 iterations from x = 5000 no longer
# converge and K is increasingly over-estimated — inherited verbatim from
# nginx (src/event/quic/ngx_event_quic_ack.c, same seed/step/break), see
# feature/quic_audit.md. The sweep below therefore stays in the converged
# domain; the K vectors pin that domain only.
K_TOLERANCE_MS = 12.0
K_CONVERGED_WMAX_MAX = 6 * 10**7


def sweep_k_tolerance():
    """Max |integer K - closed-form K| over the converged (w_max, mtu) sweep."""
    worst = 0.0
    mtu = 1200
    w_max = 2 * mtu
    while w_max <= K_CONVERGED_WMAX_MAX:
        window = tdiv(w_max * BETA, 10)
        worst = max(worst, abs(cubic_time(w_max, window, mtu) - rfc_k_ms(w_max, window, mtu)))
        w_max = int(w_max * 1.02) + 1
    for mtu in (1200, 1500):
        for window in range(2 * mtu, 200 * mtu, mtu):
            worst = max(worst, abs(cubic_time(window * 10 // 7, window, mtu)
                                   - rfc_k_ms(window * 10 // 7, window, mtu)))
    return worst


# ---------------------------------------------------------------------------
# Scenarios (step 3 inputs).
# ---------------------------------------------------------------------------

def build_scenarios():
    s = {}

    s["canonical"] = canonical_loss_state()
    canonical = s["canonical"]
    assert canonical["ssthresh"] == 7560 and canonical["window"] == 7560
    assert canonical["w_max"] == 12000

    curve_a_offsets = [
        -3000, -2103, -1500, -1000, -500, -100, 0, 100, 500, 1000, 2103, 5000,
        10000, 30000, 100000, 1000000,
    ]
    s["curve_a"] = [(t, cubic_window(t, canonical["w_max"], 1200)) for t in curve_a_offsets]

    # Second curve with a different MTU and W_max, constructed directly (the
    # enter_recovery plumbing is already pinned by curve A driving the real
    # quic_congestion_on_loss() path in the test).
    s["curve_b_wmax"], s["curve_b_window"], s["curve_b_mtu"] = 90000, 63000, 1500
    curve_b_k = cubic_time(s["curve_b_wmax"], s["curve_b_window"], s["curve_b_mtu"])
    s["curve_b_epoch"] = 100 + curve_b_k
    curve_b_offsets = [-5000, -curve_b_k, -2000, -1000, 0, 1000, curve_b_k, 10000, 50000]
    s["curve_b"] = [(t, cubic_window(t, s["curve_b_wmax"], s["curve_b_mtu"])) for t in curve_b_offsets]

    k_cases = [
        # (w_max, window, mtu) — window == beta * w_max is the post-reduction shape.
        (12000, 7560, 1200),
        (90000, 63000, 1500),
        (2000000, 1400000, 1200),
        (6000000, 4200000, 1200),
        (7560, 12000, 1200),  # w_max < window -> 0
        (12000, 12000, 1200),  # w_max == window -> 0
    ]
    s["k_solves"] = [(w_max, window, mtu, cubic_time(w_max, window, mtu))
                     for w_max, window, mtu in k_cases]

    # Congestion-avoidance runs after the canonical loss, one 1200 B packet
    # acked per step, send_time one RTT (50 ms) back:
    #
    #  - trickle: 200 acks at a 50 ms cadence, in_flight never refilled. The
    #    Reno estimate grows far slower than the curve, so the window rides
    #    the cubic chase branch and eventually the fast-ramp cap.
    #  - saturating: 1000 acks at a 10 ms cadence, in_flight refilled to the
    #    window before each ack. The Reno slope now exceeds the curve slope
    #    around t=0, so the run passes through the TCP-friendly region
    #    (window == w_est) and crosses back onto the cubic curve past K.
    rtt_ms = 50
    s["trickle_acks"], s["trickle_cadence_ms"], s["trickle_first_now"] = 200, 50, 200
    sim = canonical_loss_state()
    checkpoints = []
    for i in range(s["trickle_acks"]):
        now = s["trickle_first_now"] + s["trickle_cadence_ms"] * i
        sim["in_flight"] += 1200
        on_ack_avoid(sim, 1200, i + 1, now - rtt_ms, now, now - rtt_ms)
        if (i + 1) % 10 == 0:
            checkpoints.append((i + 1, sim["window"], sim["w_est"], sim["window"] == sim["w_est"]))
    s["trickle_run"] = checkpoints
    assert not any(c[3] for c in checkpoints), "trickle cadence must stay on the cubic curve"
    assert sim["k"] == canonical["k"], "idle=false must never shift k"
    assert sim["window"] > sim["w_max"], "past K the curve must exceed W_max"

    s["saturating_acks"], s["saturating_cadence_ms"], s["saturating_first_now"] = 1000, 10, 200
    sim = canonical_loss_state()
    checkpoints = []
    for i in range(s["saturating_acks"]):
        now = s["saturating_first_now"] + s["saturating_cadence_ms"] * i
        while sim["in_flight"] + 1200 <= sim["window"]:
            sim["in_flight"] += 1200  # quic_congestion_on_packet_sent
        on_ack_avoid(sim, 1200, i + 1, now - rtt_ms, now, now - rtt_ms)
        if (i + 1) % 50 == 0:
            checkpoints.append((i + 1, sim["window"], sim["w_est"], sim["window"] == sim["w_est"]))
    s["saturating_run"] = checkpoints
    assert sim["k"] == canonical["k"], "idle=false must never shift k"
    assert sim["w_max"] == 12000 and sim["w_prior"] == 12000
    assert any(c[3] for c in checkpoints), "saturating run must visit the TCP-friendly region"
    assert checkpoints[-1][3] is False, "final phase must be back on the cubic curve"
    assert sim["window"] > sim["w_max"], "past K the curve must exceed W_max"

    # Fast-ramp branch: a single ack long after the loss, where
    # W_cubic >= 1.5 * window, so the increase is capped at mtu/2.
    s["fast_ramp_offset_ms"] = 2000
    fast = canonical_loss_state()
    fast["in_flight"] += 1200
    ack_now = canonical["k"] + s["fast_ramp_offset_ms"]
    on_ack_avoid(fast, 1200, 1, ack_now - 50, ack_now, ack_now - 50)
    assert fast["window"] == 7560 + 600, fast["window"]
    s["fast_ramp_window"], s["fast_ramp_w_est"] = fast["window"], fast["w_est"]

    return s


# ---------------------------------------------------------------------------
# Emission (step 3).
# ---------------------------------------------------------------------------

def emit(f, s):
    w = f.write
    canonical = s["canonical"]
    w("// MACHINE-GENERATED by tests/quic_kat/gen_cubic_kat.py — do not edit.\n")
    w("// RFC 9438 CUBIC KAT vectors. The generator mirrors the integer arithmetic\n")
    w("// of quic_congestion_cubic_window/cubic_time and the avoidance branch of\n")
    w("// quic_congestion_on_ack, cross-checks every curve value against the\n")
    w("// closed-form W_cubic(t) = 0.4*(t/1000)^3*mtu + W_max and K against\n")
    w("// cbrt(W_max*(1-0.7)/0.4) before emission (tolerances cover only the\n")
    w("// generator's integer cc truncation and the Newton break, both bounded).\n")
    w("// Consumed by QuicCongestionTest.\n")
    w("#ifndef FIBER_TESTS_QUIC_CUBIC_VECTORS_H\n")
    w("#define FIBER_TESTS_QUIC_CUBIC_VECTORS_H\n\n")
    w("#include <array>\n#include <cstddef>\n#include <cstdint>\n\n")
    w("namespace fiber::quic::cubickat {\n\n")

    w("struct CubicCurvePoint {\n")
    w("    std::int64_t t_ms; // offset from the CUBIC epoch anchor cg.k\n")
    w("    std::size_t window; // expected quic_congestion_cubic_window result\n")
    w("};\n\n")
    w("struct CubicKCase {\n")
    w("    std::size_t w_max;\n    std::size_t window;\n    std::size_t mtu;\n")
    w("    std::int64_t k_ms; // expected quic_congestion_cubic_time result\n")
    w("};\n\n")
    w("struct CubicAvoidCheckpoint {\n")
    w("    std::uint32_t acks;\n    std::size_t window;\n    std::size_t w_est;\n")
    w("    bool window_is_w_est; // TCP-friendly region: window == w_est\n")
    w("};\n\n")
    w("// The vector tables below keep the generator's one-entry-per-line layout;\n")
    w("// clang-format's bin-packing of aggregate braced inits would otherwise\n")
    w("// reflow them on every regeneration.\n")
    w("// clang-format off\n")

    w("// Canonical loss state (init@0, 12000 B in flight, one 1200 B packet lost at\n")
    w("// t=100): ssthresh = window = 7560, w_max = 12000, cg.k = 100 + kCanonicalKMs.\n")
    w(f"inline constexpr std::int64_t kCanonicalKMs = {canonical['k'] - 100};\n\n")

    w(f"inline constexpr std::array<CubicCurvePoint, {len(s['curve_a'])}> kCurveA{{\n")
    for t, window in s["curve_a"]:
        w(f"        CubicCurvePoint{{{t}, {window}}},\n")
    w("};\n\n")

    w("// Direct-constructed state: mtu = 1500, w_max = 90000, window = ssthresh = 63000.\n")
    w(f"inline constexpr std::int64_t kCurveBEpochMs = {s['curve_b_epoch']};\n")
    w(f"inline constexpr std::array<CubicCurvePoint, {len(s['curve_b'])}> kCurveB{{\n")
    for t, window in s["curve_b"]:
        w(f"        CubicCurvePoint{{{t}, {window}}},\n")
    w("};\n\n")

    w(f"inline constexpr std::array<CubicKCase, {len(s['k_solves'])}> kKSolves{{\n")
    for w_max, window, mtu, k in s["k_solves"]:
        w(f"        CubicKCase{{{w_max}, {window}, {mtu}, {k}}},\n")
    w("};\n\n")

    w(f"// Trickle run: {s['trickle_acks']} acks of 1200 B at a {s['trickle_cadence_ms']} ms cadence,\n")
    w(f"// first ack at t={s['trickle_first_now']}, send_time one 50 ms RTT back; in_flight is\n")
    w("// never refilled. The Reno estimate cannot keep up with the curve, so the\n")
    w("// window rides the cubic chase branch and then the fast-ramp cap.\n")
    w(f"inline constexpr std::uint32_t kTrickleAcks = {s['trickle_acks']};\n")
    w(f"inline constexpr std::int64_t kTrickleCadenceMs = {s['trickle_cadence_ms']};\n")
    w(f"inline constexpr std::int64_t kTrickleFirstNowMs = {s['trickle_first_now']};\n")
    w(f"inline constexpr std::array<CubicAvoidCheckpoint, {len(s['trickle_run'])}> kTrickleRun{{\n")
    for acks, window, w_est, is_est in s["trickle_run"]:
        w(f"        CubicAvoidCheckpoint{{{acks}, {window}, {w_est}, {'true' if is_est else 'false'}}},\n")
    w("};\n\n")

    w(f"// Saturating run: {s['saturating_acks']} acks of 1200 B at a {s['saturating_cadence_ms']} ms cadence,\n")
    w(f"// first ack at t={s['saturating_first_now']}, send_time one 50 ms RTT back; in_flight\n")
    w("// is refilled to the window before each ack. The Reno slope now exceeds the\n")
    w("// curve slope around t=0, so the run enters the TCP-friendly region\n")
    w("// (window == w_est) and crosses back onto the cubic curve past K.\n")
    w(f"inline constexpr std::uint32_t kSaturatingAcks = {s['saturating_acks']};\n")
    w(f"inline constexpr std::int64_t kSaturatingCadenceMs = {s['saturating_cadence_ms']};\n")
    w(f"inline constexpr std::int64_t kSaturatingFirstNowMs = {s['saturating_first_now']};\n")
    w(f"inline constexpr std::array<CubicAvoidCheckpoint, {len(s['saturating_run'])}> kSaturatingRun{{\n")
    for acks, window, w_est, is_est in s["saturating_run"]:
        w(f"        CubicAvoidCheckpoint{{{acks}, {window}, {w_est}, {'true' if is_est else 'false'}}},\n")
    w("};\n\n")

    w(f"// Single ack {s['fast_ramp_offset_ms']} ms past the epoch anchor: the curve is far\n")
    w("// enough ahead that the increment is capped at mtu/2.\n")
    w(f"inline constexpr std::int64_t kFastRampOffsetMs = {s['fast_ramp_offset_ms']};\n")
    w(f"inline constexpr std::size_t kFastRampWindow = {s['fast_ramp_window']};\n")
    w(f"inline constexpr std::size_t kFastRampWEst = {s['fast_ramp_w_est']};\n\n")
    w("// clang-format on\n\n")

    w("} // namespace fiber::quic::cubickat\n\n")
    w("#endif // FIBER_TESTS_QUIC_CUBIC_VECTORS_H\n")


def main():
    s = build_scenarios()
    canonical = s["canonical"]
    for t, window in s["curve_a"]:
        check_curve_point("curveA", t, canonical["w_max"], 1200, window)
    for t, window in s["curve_b"]:
        check_curve_point("curveB", t, s["curve_b_wmax"], s["curve_b_mtu"], window)
    for w_max, window, mtu, _ in s["k_solves"]:
        if w_max > window:
            check_k("kSolve", w_max, window, mtu, cubic_time(w_max, window, mtu))

    swept = sweep_k_tolerance()
    if swept > K_TOLERANCE_MS:
        raise AssertionError(f"K sweep error {swept:.3f} ms exceeds tolerance {K_TOLERANCE_MS} ms")

    with open("tests/QuicCubicVectors.h", "w") as f:
        emit(f, s)

    est_acks = [c[0] for c in s["saturating_run"] if c[3]]
    print(f"curveA={len(s['curve_a'])} curveB={len(s['curve_b'])} kSolves={len(s['k_solves'])} "
          f"trickle={len(s['trickle_run'])} saturating={len(s['saturating_run'])} "
          f"est-region checkpoints@{est_acks} K sweep max err={swept:.3f} ms")


if __name__ == "__main__":
    main()
