#include "splatcore/quality_controller.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace splatcore {

const char* reason_name(QualityReason reason) {
    switch (reason) {
        case QualityReason::kNone:
            return "none";
        case QualityReason::kFrameMiss:
            return "frame_miss";
        case QualityReason::kThermalHigh:
            return "thermal_high";
        case QualityReason::kThermalLow:
            return "thermal_low";
        case QualityReason::kLadderExhausted:
            return "ladder_exhausted";
    }
    return "unknown";
}

QualityController::QualityController(QualityControllerConfig config) : m_config(std::move(config)) {
    if (m_config.ladder.empty()) m_config.ladder.push_back(0.0f);
    m_dwell.assign(m_config.ladder.size(), m_config.dwell_s);
}

QualityDecision QualityController::update(const QualitySample& sample) {
    const double t = sample.t_s;
    if (!m_started) {
        m_started = true;
        m_start_s = t;
        m_rung_since_s = t;
        reset_frame_window(t);
    }

    // Frame statistics for the current guard window.
    const bool guard_on = m_config.target_fps > 0.0f;
    const float period_ms = guard_on ? 1000.0f / m_config.target_fps : 0.0f;
    if (std::isfinite(sample.frame_interval_ms) && sample.frame_interval_ms > 0.0f) {
        ++m_window_frames;
        if (guard_on && sample.frame_interval_ms > m_config.miss_tolerance * period_ms) ++m_window_misses;
        if (std::isfinite(sample.gpu_ms)) {
            m_window_gpu_ms += sample.gpu_ms;
            ++m_window_gpu_frames;
        }
    }

    // Slow inputs: the clock window and the forecast slope window.
    if (sample.gpu_mhz >= 0) {
        m_clocks.push_back({t, static_cast<float>(sample.gpu_mhz)});
        m_window_max_mhz = std::max(m_window_max_mhz, sample.gpu_mhz);
        m_last_mhz = sample.gpu_mhz;
    }
    while (!m_clocks.empty() && m_clocks.front().t_s < t - m_config.clock_window_s) m_clocks.pop_front();
    bool thermal_sample = std::isfinite(sample.headroom_forecast);
    if (thermal_sample) {
        if (t - m_last_forecast_s > m_config.forecast_gap_s) m_forecaster_since_s = t;
        m_last_forecast_s = t;
        m_forecast = sample.headroom_forecast;
        thermal_sample = t - m_forecaster_since_s >= m_config.forecast_warmup_s;
        if (thermal_sample) m_forecasts.push_back({t, sample.headroom_forecast});
    }
    while (!m_forecasts.empty() && m_forecasts.front().t_s < t - m_config.slope_window_s)
        m_forecasts.pop_front();
    if (std::isfinite(sample.headroom_current)) {
        m_current = sample.headroom_current;
        m_last_current_s = t;
    }

    // Frame guard: one evaluation per window, so at most one step per window.
    if (guard_on && t - m_window_start_s >= m_config.frame_window_s) {
        const bool gpu_bound = m_window_gpu_frames > 0 && m_window_gpu_ms / m_window_gpu_frames >=
                                                              m_config.gpu_bound_fraction * period_ms;
        const bool missed = m_window_frames >= m_config.min_window_frames &&
                            m_window_misses > m_config.miss_fraction * static_cast<float>(m_window_frames);
        const int window_mhz = m_window_max_mhz >= 0 ? m_window_max_mhz : m_last_mhz;
        const bool no_headroom =
            m_config.frame_guard_clock_fraction <= 0.0f || m_config.max_gpu_mhz <= 0 || window_mhz < 0 ||
            static_cast<float>(window_mhz) >=
                m_config.frame_guard_clock_fraction * static_cast<float>(m_config.max_gpu_mhz);
        reset_frame_window(t);
        if (t - m_start_s >= m_config.warmup_s) {
            const int span = std::clamp(m_config.bad_window_span, 1, 32);
            const std::uint32_t mask = span == 32 ? ~0u : (1u << span) - 1u;
            m_bad_windows = ((m_bad_windows << 1) | (missed && gpu_bound && no_headroom ? 1u : 0u)) & mask;
            if (std::popcount(m_bad_windows) >= m_config.bad_windows_to_step) {
                const QualityDecision decision = step_down(t, QualityReason::kFrameMiss);
                if (decision.reason != QualityReason::kNone) return decision;
            }
        }
    }

    // Thermal loop, on each new forecast.
    if (thermal_sample) {
        const float light = m_config.light_headroom;
        if (m_forecast >= light - m_config.step_down_margin) {
            const float slope = slope_per_min();
            const bool falling = std::isfinite(slope) && slope < 0.0f;  // no slope yet counts as rising
            if (!falling && t - m_last_step_s >= m_config.thermal_interval_s) {
                return step_down(t, QualityReason::kThermalHigh);
            }
        } else if (m_rung > 0 && m_forecast <= light - m_config.step_up_margin &&
                   t - m_rung_since_s >= m_dwell[static_cast<std::size_t>(m_rung)] &&
                   clock_allows_step_up() && current_allows_step_up(t)) {
            return step_up(t);
        }
    }
    return {m_rung, QualityReason::kNone, false};
}

QualityDecision QualityController::step_down(double t_s, QualityReason reason) {
    if (m_rung + 1 >= rung_count()) {
        // Reported at the thermal loop's pace, not every second the frame guard fires.
        if (t_s - m_last_exhausted_s < m_config.thermal_interval_s)
            return {m_rung, QualityReason::kNone, false};
        m_last_exhausted_s = t_s;
        return {m_rung, QualityReason::kLadderExhausted, false};
    }
    const int next = m_rung + 1;
    if (m_last_up_from == next && t_s - m_last_up_s <= m_config.backoff_window_s) {
        // The step up from `next` did not hold: wait longer there before trying again.
        double& dwell = m_dwell[static_cast<std::size_t>(next)];
        dwell = std::min(2.0 * dwell, m_config.max_dwell_s);
    }
    m_rung = next;
    m_rung_since_s = t_s;
    m_last_step_s = t_s;
    reset_frame_window(t_s);
    m_bad_windows = 0;  // the new rung starts with a clean history
    return {m_rung, reason, true};
}

QualityDecision QualityController::step_up(double t_s) {
    m_last_up_from = m_rung;
    m_last_up_s = t_s;
    m_rung -= 1;
    m_rung_since_s = t_s;
    m_last_step_s = t_s;
    reset_frame_window(t_s);
    m_bad_windows = 0;
    return {m_rung, QualityReason::kThermalLow, true};
}

void QualityController::reset_frame_window(double t_s) {
    m_window_start_s = t_s;
    m_window_frames = 0;
    m_window_misses = 0;
    m_window_gpu_frames = 0;
    m_window_gpu_ms = 0.0;
    m_window_max_mhz = -1;
}

bool QualityController::clock_allows_step_up() const {
    if (m_config.max_gpu_mhz <= 0) return true;
    const float mhz = clock_mhz();
    if (!std::isfinite(mhz)) return true;  // the clock is unreadable: the dwell alone gates
    return mhz < m_config.step_up_clock_fraction * static_cast<float>(m_config.max_gpu_mhz);
}

bool QualityController::current_allows_step_up(double t_s) const {
    if (!m_config.step_up_needs_current || !std::isfinite(m_current)) return true;
    if (t_s - m_last_current_s > m_config.forecast_gap_s) return true;  // stale: the forecast decides
    return m_current <= m_config.light_headroom - m_config.step_up_margin;
}

float QualityController::slope_per_min() const {
    if (m_forecasts.size() < 3 ||
        m_forecasts.back().t_s - m_forecasts.front().t_s < m_config.min_slope_span_s) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    // Least squares on times relative to the window start (keeps the sums well conditioned).
    const double t0 = m_forecasts.front().t_s;
    double st = 0.0, sv = 0.0, stt = 0.0, stv = 0.0;
    for (const TimedValue& s : m_forecasts) {
        const double dt = s.t_s - t0;
        st += dt;
        sv += s.value;
        stt += dt * dt;
        stv += dt * s.value;
    }
    const double n = static_cast<double>(m_forecasts.size());
    const double denominator = n * stt - st * st;
    if (denominator <= 0.0) return std::numeric_limits<float>::quiet_NaN();
    return static_cast<float>(60.0 * (n * stv - st * sv) / denominator);
}

float QualityController::clock_mhz() const {
    if (m_clocks.empty()) return std::numeric_limits<float>::quiet_NaN();
    double sum = 0.0;
    for (const TimedValue& s : m_clocks) sum += s.value;
    return static_cast<float>(sum / static_cast<double>(m_clocks.size()));
}

}  // namespace splatcore
