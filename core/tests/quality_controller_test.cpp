#include "splatcore/quality_controller.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

namespace splatcore {
namespace {

struct Event {
    double t_s;
    QualityDecision decision;
};

// A frame source and thermal plant that answer the controller's current rung: GPU-bound frames
// miss the 60 fps target (50 fps, 19.5 ms GPU) when `misses(t, rung)`, and once per second the
// plant reports a headroom forecast, a current headroom (NaN = none) and a GPU clock.
struct Plant {
    float target_fps = 60.0f;
    std::function<bool(double, int)> misses = [](double, int) { return false; };
    std::function<float(double, int)> forecast = [](double, int) { return 0.6f; };
    std::function<float(double, int)> current = [](double, int) {
        return std::numeric_limits<float>::quiet_NaN();
    };
    std::function<int(double, int)> mhz = [](double, int) { return -1; };
};

std::vector<Event> simulate(QualityController& controller, double seconds, const Plant& plant) {
    std::vector<Event> events;
    double next_thermal = 0.0;
    for (double t = 0.0; t < seconds;) {
        const bool miss = plant.misses(t, controller.rung());
        const float interval_ms = miss ? 20.0f : 1000.0f / plant.target_fps;
        QualitySample sample;
        sample.t_s = t;
        sample.frame_interval_ms = interval_ms;
        sample.gpu_ms = miss ? 19.5f : 12.0f;
        if (t >= next_thermal) {
            sample.headroom_forecast = plant.forecast(t, controller.rung());
            sample.headroom_current = plant.current(t, controller.rung());
            sample.gpu_mhz = plant.mhz(t, controller.rung());
            next_thermal += 1.0;
        }
        const QualityDecision decision = controller.update(sample);
        if (decision.reason != QualityReason::kNone) events.push_back({t, decision});
        t += interval_ms / 1000.0;
    }
    return events;
}

std::vector<Event> changes(const std::vector<Event>& events) {
    std::vector<Event> out;
    for (const Event& e : events) {
        if (e.decision.changed) out.push_back(e);
    }
    return out;
}

TEST(QualityController, ReasonNamesMatchTheDecisionLog) {
    EXPECT_STREQ(reason_name(QualityReason::kNone), "none");
    EXPECT_STREQ(reason_name(QualityReason::kFrameMiss), "frame_miss");
    EXPECT_STREQ(reason_name(QualityReason::kThermalHigh), "thermal_high");
    EXPECT_STREQ(reason_name(QualityReason::kThermalLow), "thermal_low");
    EXPECT_STREQ(reason_name(QualityReason::kLadderExhausted), "ladder_exhausted");
}

TEST(QualityController, StartsOnTheBestRung) {
    QualityController controller;
    EXPECT_EQ(controller.rung(), 0);
    EXPECT_EQ(controller.rung_count(), 3);
    EXPECT_FLOAT_EQ(controller.min_contribution(), 0.1f);
    QualityControllerConfig empty;
    empty.ladder.clear();
    EXPECT_EQ(QualityController(empty).rung_count(), 1);  // an empty ladder is one rung (no culling)
}

TEST(QualityController, FrameGuardStepsDownAfterFourBadWindowsOfSix) {
    QualityController controller;
    Plant plant;
    plant.misses = [](double, int) { return true; };  // every rung misses: walks the ladder, then reports
    const std::vector<Event> events = simulate(controller, 30.0, plant);
    ASSERT_EQ(events.size(), 3u);
    // The window closing at ~1 s falls in the 2 s warm-up; those at ~2..5 s are the four bad ones.
    // Each step starts a fresh history, and the exhausted ladder is reported once per thermal
    // interval, not every second.
    EXPECT_EQ(events[0].decision.reason, QualityReason::kFrameMiss);
    EXPECT_EQ(events[0].decision.rung, 1);
    // (A window closes on the first frame past its second: 50 fps windows run ~1.02 s.)
    EXPECT_NEAR(events[0].t_s, 5.0, 0.25);
    EXPECT_EQ(events[1].decision.rung, 2);
    EXPECT_NEAR(events[1].t_s, 9.0, 0.25);
    EXPECT_EQ(events[2].decision.reason, QualityReason::kLadderExhausted);
    EXPECT_FALSE(events[2].decision.changed);
    EXPECT_NEAR(events[2].t_s, 13.0, 0.25);
}

TEST(QualityController, FrameGuardRidesOutAGovernorDip) {
    // Recorded on both phones: the clock governor drops the clock for a few seconds and nearly
    // every frame misses meanwhile, then recovers. The longest dip (S24U stump 734x1591, 50-52 s,
    // 500 MHz) missed 49 + 45 + 22 frames: ~2.4 s, at any phase against the windows.
    QualityController controller;
    Plant plant;
    plant.misses = [](double t, int) { return (t >= 30.0 && t < 32.4) || (t >= 90.7 && t < 93.1); };
    EXPECT_TRUE(simulate(controller, 120.0, plant).empty());
}

TEST(QualityController, FrameGuardCatchesARecurringOverload) {
    // A rung that cannot hold the target on part of the orbit: two bad seconds out of every three.
    QualityController controller;
    Plant plant;
    plant.misses = [](double t, int rung) { return rung == 0 && std::fmod(t, 3.0) < 2.0; };
    const std::vector<Event> events = simulate(controller, 30.0, plant);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].decision.reason, QualityReason::kFrameMiss);
    EXPECT_LT(events[0].t_s, 10.0);
}

TEST(QualityController, FrameGuardWaitsForAClockWithoutHeadroom) {
    // S21 hand-held session (S24U, stump): a fifth of the frames were late at every rung and every
    // clock, with the governor between 231 and 770 MHz of 1000. A GPU below 0.9 of its maximum
    // clock has capacity left, so its late frames are not a reason to cut quality (ADR-0011).
    for (const int mhz : {770, 950}) {
        QualityControllerConfig config;
        config.max_gpu_mhz = 1000;
        QualityController controller(config);
        Plant plant;
        plant.misses = [](double, int) { return true; };
        plant.mhz = [mhz](double, int) { return mhz; };
        const std::vector<Event> events = simulate(controller, 30.0, plant);
        if (mhz == 770) {
            EXPECT_TRUE(events.empty());
        } else {
            ASSERT_FALSE(events.empty());
            EXPECT_EQ(events[0].decision.reason, QualityReason::kFrameMiss);
            EXPECT_NEAR(events[0].t_s, 5.0, 0.25);  // as without the condition
        }
    }
}

TEST(QualityController, FrameGuardIgnoresMissesTheGpuDidNotCause) {
    QualityController controller;
    double t = 0.0;
    for (int i = 0; i < 1500; ++i) {  // 30 s of 20 ms frames with 8 ms of GPU work: a CPU stall
        QualitySample sample;
        sample.t_s = t;
        sample.frame_interval_ms = 20.0f;
        sample.gpu_ms = 8.0f;
        EXPECT_EQ(controller.update(sample).reason, QualityReason::kNone);
        t += 0.02;
    }
    EXPECT_EQ(controller.rung(), 0);
}

TEST(QualityController, FrameGuardIsOffWithoutATarget) {
    QualityControllerConfig config;
    config.target_fps = 0.0f;  // uncapped bench: only the thermal loop acts
    QualityController controller(config);
    Plant plant;
    plant.misses = [](double, int) { return true; };
    EXPECT_TRUE(simulate(controller, 30.0, plant).empty());
}

TEST(QualityController, UncountedFramesDecideNothing) {
    QualityController controller;
    double t = 0.0;
    for (int i = 0; i < 300; ++i) {  // on-demand frames after idle gaps carry no interval
        QualitySample sample;
        sample.t_s = t;
        sample.gpu_ms = 30.0f;
        EXPECT_EQ(controller.update(sample).reason, QualityReason::kNone);
        t += 0.1;
    }
}

TEST(QualityController, HotStartStepsDownAfterTheForecastWarmUpThenOncePerInterval) {
    QualityController controller;
    Plant plant;
    plant.forecast = [](double, int) { return 0.85f; };  // above LIGHT - 0.017 = 0.816, flat
    const std::vector<Event> events = simulate(controller, 220.0, plant);
    ASSERT_EQ(events.size(), 4u);
    // No slope yet counts as rising, so the first forecast past the warm-up steps; a flat forecast
    // (slope 0) is not falling, so the loop keeps stepping at its 60 s pace until the ladder runs
    // out. Each gap is 60 s rounded up to the next 1 Hz forecast sample.
    EXPECT_EQ(events[0].decision.reason, QualityReason::kThermalHigh);
    EXPECT_NEAR(events[0].t_s, 20.0, 0.05);
    EXPECT_EQ(events[1].decision.rung, 2);
    EXPECT_EQ(events[2].decision.reason, QualityReason::kLadderExhausted);
    EXPECT_EQ(events[3].decision.reason, QualityReason::kLadderExhausted);
    for (std::size_t i = 1; i < events.size(); ++i) {
        const double gap = events[i].t_s - events[i - 1].t_s;
        EXPECT_GE(gap, 60.0) << "event " << i;
        EXPECT_LE(gap, 61.1) << "event " << i;
    }
}

TEST(QualityController, ForecastWarmUpIgnoresThePlatformStartUpSpike) {
    // S24U smoke test (684f4fa, stump at 60 fps from skin 31 C): the platform's first 30 s
    // forecasts, every 2 s, before its sample history settled. The old policy stepped at 2 s.
    const float recorded[] = {0.533f, 1.066f, 0.766f, 0.673f, 0.633f, 0.612f, 0.684f, 0.692f};
    QualityController controller;
    Plant plant;
    plant.forecast = [&recorded](double t, int) {
        const auto i = static_cast<std::size_t>(t / 2.0);
        if (std::fmod(t, 2.0) >= 1.0) return std::numeric_limits<float>::quiet_NaN();  // current headroom
        return i < std::size(recorded) ? recorded[i] : 0.69f;
    };
    // By 50 s the slope window (60 s) would still hold the spike if it had entered: steeply falling.
    EXPECT_TRUE(simulate(controller, 50.0, plant).empty());
    EXPECT_NEAR(controller.slope_per_min(), 0.0f, 1e-3f);
}

TEST(QualityController, AForecastGapRestartsTheWarmUp) {
    // The platform forgets its samples after 10 s without a call (e.g. an idle on-demand view):
    // the next forecasts start from a short history again.
    QualityController controller;
    Plant plant;
    plant.forecast = [](double t, int) {
        if (t >= 60.0 && t < 90.0) return std::numeric_limits<float>::quiet_NaN();  // no calls
        return t >= 92.0 && t < 93.0 ? 1.0f : 0.70f;
    };
    EXPECT_TRUE(simulate(controller, 150.0, plant).empty());

    QualityController steady;  // the same spike without a gap is a real reading and steps
    plant.forecast = [](double t, int) { return t >= 92.0 && t < 93.0 ? 1.0f : 0.70f; };
    const std::vector<Event> events = simulate(steady, 150.0, plant);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_NEAR(events[0].t_s, 92.0, 0.05);
}

TEST(QualityController, FallingForecastHoldsTheRung) {
    QualityController controller;
    Plant plant;
    // Above the band but cooling at 0.02 per minute (the rung already cut the power): no second step.
    plant.forecast = [](double t, int) { return 0.88f - 0.02f * static_cast<float>(t / 60.0); };
    const std::vector<Event> events = simulate(controller, 180.0, plant);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].decision.rung, 1);
    EXPECT_LT(controller.slope_per_min(), 0.0f);
    EXPECT_NEAR(controller.slope_per_min(), -0.02f, 1e-3f);
}

TEST(QualityController, StepUpWaitsForTheDwellAndTheClock) {
    for (const int mhz : {700, 500}) {
        QualityControllerConfig config;
        config.max_gpu_mhz = 1000;  // gate: step up only below 600 MHz
        QualityController controller(config);
        Plant plant;
        plant.forecast = [](double t, int) { return t < 21.0 ? 0.85f : 0.70f; };  // hot, then cool
        plant.mhz = [mhz](double, int) { return mhz; };
        const std::vector<Event> events = changes(simulate(controller, 300.0, plant));
        if (mhz == 700) {
            ASSERT_EQ(events.size(), 1u) << "a busy clock blocks the step up";
            EXPECT_EQ(controller.rung(), 1);
            EXPECT_NEAR(controller.clock_mhz(), 700.0f, 1e-3f);
        } else {
            ASSERT_EQ(events.size(), 2u);
            EXPECT_EQ(events[1].decision.reason, QualityReason::kThermalLow);
            EXPECT_EQ(events[1].decision.rung, 0);
            EXPECT_NEAR(events[1].t_s - events[0].t_s, 60.0, 1.0);  // the dwell at rung 1, not earlier
        }
    }
}

TEST(QualityController, StepUpWithoutAClockReadingWaitsForTheDwellOnly) {
    QualityControllerConfig config;
    config.max_gpu_mhz = 1000;
    QualityController controller(config);
    Plant plant;
    plant.forecast = [](double t, int) { return t < 21.0 ? 0.85f : 0.70f; };  // clock unreadable (-1)
    const std::vector<Event> events = changes(simulate(controller, 120.0, plant));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_NEAR(events[1].t_s - events[0].t_s, 60.0, 1.0);
}

TEST(QualityController, AStepUpThatDoesNotHoldDoublesTheDwellUpToTheCap) {
    QualityController controller;
    Plant plant;
    plant.misses = [](double, int rung) { return rung == 0; };  // rung 0 cannot hold the target
    plant.forecast = [](double, int) { return 0.70f; };         // cool: the thermal loop keeps trying
    const std::vector<Event> events = changes(simulate(controller, 2200.0, plant));
    // Down at ~5 s, then up after each dwell and back down by the frame guard 4 s later.
    ASSERT_GE(events.size(), 11u);
    EXPECT_EQ(events[0].decision.reason, QualityReason::kFrameMiss);
    const double expected_dwell[] = {60.0, 120.0, 240.0, 480.0, 600.0};
    for (int i = 0; i < 5; ++i) {
        const Event& down = events[static_cast<std::size_t>(2 * i)];
        const Event& up = events[static_cast<std::size_t>(2 * i + 1)];
        EXPECT_EQ(down.decision.rung, 1);
        EXPECT_EQ(up.decision.reason, QualityReason::kThermalLow);
        EXPECT_NEAR(up.t_s - down.t_s, expected_dwell[i], 1.5) << "cycle " << i;
    }
    EXPECT_DOUBLE_EQ(controller.dwell_s(1), 600.0);
    EXPECT_DOUBLE_EQ(controller.dwell_s(0), 60.0);  // only the rung that was left backs off
}

TEST(QualityController, StepUpWaitsForTheCurrentHeadroomToo) {
    // S25U A/B (S20) at 610 s: the 30 s forecast extrapolated the skin sensor's last steps to
    // 0.747 while the current headroom read 0.770 - 1.9 C below LIGHT, not the 2.5 C the step-up
    // band asks for. The step up was undone 242 s later. Since ADR-0011 both readings must be in
    // the band.
    for (const bool needs_current : {true, false}) {
        QualityControllerConfig config;
        config.step_up_needs_current = needs_current;
        QualityController controller(config);
        Plant plant;
        plant.forecast = [](double t, int) { return t < 21.0 ? 0.85f : 0.74f; };
        plant.current = [](double t, int) { return t < 21.0 ? 0.84f : t < 150.0 ? 0.78f : 0.74f; };
        const std::vector<Event> events = changes(simulate(controller, 200.0, plant));
        ASSERT_EQ(events.size(), 2u);
        EXPECT_EQ(events[1].decision.reason, QualityReason::kThermalLow);
        // With the gate the step up waits for the current reading to enter the band at 150 s;
        // without it the forecast alone steps up as soon as the 60 s dwell has passed.
        EXPECT_NEAR(events[1].t_s, needs_current ? 150.0 : 80.0, 1.5);
    }
}

TEST(QualityController, AStaleCurrentReadingLeavesTheStepUpToTheForecast) {
    QualityController controller;
    Plant plant;
    plant.forecast = [](double t, int) { return t < 21.0 ? 0.85f : 0.74f; };
    plant.current = [](double t, int) {  // last reading hot, then none (an older recording)
        return t < 30.0 ? 0.80f : std::numeric_limits<float>::quiet_NaN();
    };
    const std::vector<Event> events = changes(simulate(controller, 200.0, plant));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_NEAR(events[1].t_s, 80.0, 1.5);  // the dwell, not the stale 0.80
}

TEST(QualityController, AStepUpUndoneAfterFourMinutesBacksOff) {
    // The S25U A/B's step up (S20): the forecast rose within ~100 s of it and a slow drift carried
    // it over the band 242 s after it. ADR-0009's 120 s backoff window missed that; ADR-0011's
    // 600 s window doubles the dwell of the rung the step up left.
    for (const double window : {120.0, 600.0}) {
        QualityControllerConfig config;
        config.backoff_window_s = window;
        QualityController controller(config);
        double up_at = -1.0;
        Plant plant;
        plant.forecast = [&up_at](double t, int rung) {
            if (t < 21.0) return 0.85f;  // hot start: one step down at 20 s
            if (rung == 1) return 0.70f;
            if (up_at < 0.0) up_at = t;
            return 0.70f + 0.15f * static_cast<float>((t - up_at) / 300.0);  // over the band at +232 s
        };
        const std::vector<Event> events = changes(simulate(controller, 360.0, plant));
        ASSERT_EQ(events.size(), 3u);
        EXPECT_EQ(events[1].decision.reason, QualityReason::kThermalLow);
        EXPECT_EQ(events[2].decision.reason, QualityReason::kThermalHigh);
        EXPECT_NEAR(events[2].t_s - events[1].t_s, 232.0, 2.0);
        EXPECT_DOUBLE_EQ(controller.dwell_s(1), window > 232.0 ? 120.0 : 60.0) << "window " << window;
    }
}

// First-order skin temperature toward a per-rung steady state. The steady states are ADR-0009's
// S24U prediction for stump at 60 fps and 864x1872: 30.8 C + 2.5 C/W x power -> c 0.1 ~44 C,
// c 0.2 ~41.5 C, c 0.5 ~38.7 C. Headroom = (T - 15) / 30; the forecast extrapolates 30 s.
struct SkinModel {
    double steady_c[3];
    double tau_s = 240.0;
    double temp_c = 33.0;
    double last_t = 0.0;
    double peak_headroom = 0.0;
    float operator()(double t, int rung) {
        const double dt = t - last_t;
        last_t = t;
        const double target = steady_c[rung];
        temp_c += (target - temp_c) * (1.0 - std::exp(-dt / tau_s));
        const double headroom = (temp_c - 15.0) / 30.0;
        peak_headroom = std::max(peak_headroom, headroom);
        const double slope_c_per_s = (target - temp_c) / tau_s;
        return static_cast<float>((temp_c + 30.0 * slope_c_per_s - 15.0) / 30.0);
    }
};

TEST(QualityController, HeatModelSettlesOnTheRungThatStaysBelowLight) {
    QualityController controller;
    SkinModel model{{44.0, 41.5, 38.7}};
    Plant plant;
    plant.forecast = [&model](double t, int rung) { return model(t, rung); };
    const std::vector<Event> events = changes(simulate(controller, 600.0, plant));
    // Prediction to test on the phone: c 0.1 -> c 0.2 -> c 0.5, then no more changes.
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].decision.reason, QualityReason::kThermalHigh);
    EXPECT_GE(events[1].t_s - events[0].t_s, 60.0);
    EXPECT_EQ(controller.rung(), 2);
    EXPECT_LT(model.peak_headroom, 0.85);            // overshoot past LIGHT (0.833) stays small
    EXPECT_LT((model.temp_c - 15.0) / 30.0, 0.833);  // and the plateau sits below LIGHT

    // The same plant at a fixed c 0.1 ends past MODERATE (0.90).
    SkinModel fixed{{44.0, 41.5, 38.7}};
    for (double t = 0.0; t < 600.0; t += 1.0) fixed(t, 0);
    EXPECT_GT((fixed.temp_c - 15.0) / 30.0, 0.90);
}

TEST(QualityController, CoolSceneNeverLeavesTheBestRung) {
    QualityController controller;
    SkinModel model{{38.0, 37.0, 36.0}};  // a light scene: the best rung settles below the band
    Plant plant;
    plant.forecast = [&model](double t, int rung) { return model(t, rung); };
    EXPECT_TRUE(simulate(controller, 600.0, plant).empty());
}

}  // namespace
}  // namespace splatcore
