#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace splatcore {

// Thermal-adaptive quality controller (ADR-0009). It moves one index along a measured ladder of
// contribution-cull thresholds so that a frame-rate target holds without reaching thermal
// throttling. Pure policy: the host samples the platform (frame intervals, GPU timestamps, the
// Android thermal headroom forecast, the GPU clock), feeds one QualitySample per rendered frame
// and applies the returned rung. The same object replays a recorded bench CSV offline
// (`splatconv replay`).
//
// Two loops:
//  * Frame guard (fast). A one-second window is bad when more than 10 % of its frames missed the
//    target interval, the GPU time averaged at least 90 % of it and the GPU clock was at least
//    0.9 of its maximum: the GPU was the bottleneck with no clock headroom left, not a CPU or UI
//    hitch or the governor's choice of clock (ADR-0011: in hand-held use a fifth of the frames
//    were late at every rung and clock). Step down when 4 of the last 6 windows were bad:
//    governors also drop the clock for 1-3 s now and then and every frame misses meanwhile
//    (seen in recorded runs on both phones, ADR-0009), while a rung that cannot hold the target
//    misses window after window.
//  * Thermal loop (slow), on the 30 s headroom forecast. Step down when the forecast reaches the
//    LIGHT threshold minus ~0.5 C and is still rising over the last 60 s, at most once per 60 s
//    after any step: skin temperature answers a power change only over minutes, so a faster loop
//    would keep stepping while heat from earlier frames arrives. Step up when the forecast and the
//    current headroom are both ~2.5 C below LIGHT, the current rung's dwell has passed and the GPU
//    clock is below 60 % of its maximum: the forecast extrapolates a falling skin, so alone it
//    can call a phone cool that is not (ADR-0011). A step up that is undone within 600 s - the
//    skin's fast response plus the slow drift that carries a failed step up over the band -
//    doubles the dwell of the rung it left.
//    Forecasts from the platform forecaster's first 20 s are not used: it fits its slope to as few
//    as three one-second skin samples, so one 1 C sensor step reads as +15 C in 30 s.

enum class QualityReason : std::uint8_t {
    kNone = 0,         // no decision on this sample
    kFrameMiss,        // frame guard: GPU-bound frames missed the target -> one rung down
    kThermalHigh,      // forecast in the band below LIGHT and not falling -> one rung down
    kThermalLow,       // forecast well below LIGHT, dwell passed, clock has room -> one rung up
    kLadderExhausted,  // a step down was due on the last rung: reported, nothing changes
};

// Log names (bench CSV, logcat): "none", "frame_miss", "thermal_high", "thermal_low",
// "ladder_exhausted".
const char* reason_name(QualityReason reason);

struct QualityControllerConfig {
    // Contribution-cull thresholds in pixels of the phone reference image (ADR-0008,
    // SplatRenderer::set_min_contribution), best quality first. Rung order rule: each step down
    // saves the most energy per frame for each dB it costs (ADR-0009 decision 1).
    std::vector<float> ladder{0.1f, 0.2f, 0.5f};
    float target_fps = 60.0f;  // frame-guard target; <= 0 turns the frame guard off

    // Frame guard.
    double frame_window_s = 1.0;
    float miss_tolerance = 1.1f;       // an interval above this x the target interval is a miss
    float miss_fraction = 0.10f;       // a window is bad when more than this fraction missed
    float gpu_bound_fraction = 0.90f;  // ... and the mean GPU time is >= this x the target interval
    int min_window_frames = 5;         // windows with fewer counted frames are not bad
    int bad_windows_to_step = 4;       // step down when this many of the last
    int bad_window_span = 6;           // ... this many windows (<= 32) were bad
    // ADR-0011: a window is bad only while the GPU clock is at least this x max_gpu_mhz (the
    // highest reading in the window, else the last one). Below it the governor has headroom
    // left and the frame times are its choice: in hand-held use on the S24U a fifth of the
    // frames were late at every rung and every clock. 0, an unknown maximum or no reading yet
    // leave the guard without this condition (ADR-0009, and older recordings).
    float frame_guard_clock_fraction = 0.9f;
    double warmup_s = 2.0;  // windows closing this soon after the first sample are ignored

    // Thermal loop. Headroom 1.0 = SEVERE; on the Samsung skin sensor it moves 1/30 per C.
    float light_headroom = 0.833f;     // LIGHT threshold; default = (40 C - 15) / 30 (S24U, S25U)
    float step_down_margin = 0.017f;   // step down at forecast >= light - margin (~0.5 C)
    float step_up_margin = 0.083f;     // step up at forecast <= light - margin (~2.5 C)
    double thermal_interval_s = 60.0;  // minimum time from any step to a thermal step down
    double slope_window_s = 60.0;      // forecast slope = least squares over this window
    double min_slope_span_s = 20.0;    // shorter histories have no slope; counted as rising
    // Android's forecaster fits a least-squares slope to its 1 Hz skin samples (up to 30) and
    // extrapolates it. One 1 C sensor step at the newest of n samples moves a 30 s forecast by
    // 180 / (n (n + 1)) C: 15 C at 3 samples (seen on the S24U at 2 s), 0.43 C at 20. Forecasts
    // this soon after the forecaster started are logged but neither act nor enter the slope.
    double forecast_warmup_s = 20.0;
    double forecast_gap_s = 10.0;  // Android drops its samples after 10 s without a call: restart
    double dwell_s = 60.0;         // initial minimum time at a rung before a step up
    double max_dwell_s = 600.0;
    // A step down this soon after a step up doubles the dwell. ADR-0011: 600 s (ADR-0009 had
    // 120 s, which a step up undone after 242 s on the S25U escaped).
    double backoff_window_s = 600.0;
    // ADR-0011: a step up also needs the current headroom at or below the step-up threshold. A
    // current reading older than forecast_gap_s, or none at all, leaves the forecast alone to
    // decide (ADR-0009 behaviour, and the replay of controller runs recorded before ADR-0011).
    bool step_up_needs_current = true;
    float step_up_clock_fraction = 0.60f;  // step up only below this x max_gpu_mhz
    double clock_window_s = 10.0;          // the clock gate uses the mean over this window
    int max_gpu_mhz = 0;                   // <= 0 = unknown: step ups wait for the dwell alone
};

struct QualitySample {
    double t_s = 0.0;  // monotonic time of the frame, any origin
    // Time since the previous rendered frame; NaN or <= 0 = not counted (first frame, or the
    // first one after an idle gap of on-demand rendering).
    float frame_interval_ms = std::numeric_limits<float>::quiet_NaN();
    // GPU time of a recent frame (timestamps trail by the frames in flight); NaN = unknown.
    float gpu_ms = std::numeric_limits<float>::quiet_NaN();
    // A new headroom forecast taken with this frame, NaN = none (the platform allows ~1 Hz).
    float headroom_forecast = std::numeric_limits<float>::quiet_NaN();
    // A new current (0 s) headroom reading taken with this frame, NaN = none. The Android host
    // alternates it with the forecast, one platform call per second.
    float headroom_current = std::numeric_limits<float>::quiet_NaN();
    int gpu_mhz = -1;  // a new GPU clock reading taken with this frame, -1 = none
};

struct QualityDecision {
    int rung = 0;  // the rung to render with from the next frame on
    QualityReason reason = QualityReason::kNone;
    bool changed = false;  // false for kNone and kLadderExhausted
};

class QualityController {
public:
    explicit QualityController(QualityControllerConfig config = {});

    QualityDecision update(const QualitySample& sample);

    int rung() const { return m_rung; }
    int rung_count() const { return static_cast<int>(m_config.ladder.size()); }
    float min_contribution() const { return m_config.ladder[static_cast<std::size_t>(m_rung)]; }
    const QualityControllerConfig& config() const { return m_config; }

    // Diagnostics for the decision log; NaN until known.
    float forecast() const { return m_forecast; }
    float current() const { return m_current; }  // last current headroom reading
    float slope_per_min() const;                 // least-squares forecast slope over the slope window
    float clock_mhz() const;                     // mean GPU clock over the clock window
    double dwell_s(int rung) const { return m_dwell[static_cast<std::size_t>(rung)]; }

private:
    struct TimedValue {
        double t_s;
        float value;
    };
    QualityDecision step_down(double t_s, QualityReason reason);
    QualityDecision step_up(double t_s);
    void reset_frame_window(double t_s);
    bool clock_allows_step_up() const;
    bool current_allows_step_up(double t_s) const;

    QualityControllerConfig m_config;
    int m_rung = 0;
    std::vector<double> m_dwell;  // per rung: time to stay before stepping up from it

    bool m_started = false;
    double m_start_s = 0.0;
    double m_rung_since_s = 0.0;
    double m_last_step_s = -std::numeric_limits<double>::infinity();
    double m_last_exhausted_s = -std::numeric_limits<double>::infinity();
    double m_last_up_s = -std::numeric_limits<double>::infinity();
    int m_last_up_from = -1;  // rung the last step up left

    std::uint32_t m_bad_windows = 0;  // one bit per closed window since the last step, newest = bit 0
    double m_window_start_s = 0.0;
    int m_window_frames = 0;
    int m_window_misses = 0;
    int m_window_gpu_frames = 0;
    double m_window_gpu_ms = 0.0;
    int m_window_max_mhz = -1;  // highest clock reading in the window, -1 = none
    int m_last_mhz = -1;

    float m_forecast = std::numeric_limits<float>::quiet_NaN();
    double m_last_forecast_s = -std::numeric_limits<double>::infinity();
    double m_forecaster_since_s = 0.0;  // first forecast after the last gap
    float m_current = std::numeric_limits<float>::quiet_NaN();
    double m_last_current_s = -std::numeric_limits<double>::infinity();
    std::deque<TimedValue> m_forecasts;  // slope window
    std::deque<TimedValue> m_clocks;     // clock window
};

}  // namespace splatcore
