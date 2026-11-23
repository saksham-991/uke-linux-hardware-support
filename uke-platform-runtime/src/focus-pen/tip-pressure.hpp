// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <optional>

class TipPressure {
public:
    using Stamp = std::chrono::steady_clock::time_point;
    // The live lift test showed a 37 ms gap during a sustained stroke.
    static constexpr auto max_gap = std::chrono::milliseconds(45);

    bool sample(int pressure, Stamp now) {
        // This pen continues reporting low nonzero pressure during hover.
        // Hysteresis avoids re-arming the tip on the same residual readings.
        if (pressure < (value_ ? lift_cutoff : touch_cutoff))
            pressure = 0;
        const bool lifted = value_ && !pressure;
        if (lifted)
            last_lift_ = now;
        value_ = pressure;
        updated_ = now;
        return lifted;
    }

    bool expire(Stamp now) {
        if (!value_ || now < updated_ + max_gap)
            return false;
        return sample(0, now);
    }

    std::optional<Stamp> deadline() const {
        if (value_)
            return updated_ + max_gap;
        return std::nullopt;
    }

    int for_frame(Stamp received, Stamp now) const {
        return received < last_lift_ || now >= updated_ + max_gap ? 0 : value_;
    }

private:
    static constexpr int lift_cutoff = 170;
    static constexpr int touch_cutoff = 192;
    Stamp updated_{}, last_lift_{};
    int value_ = 0;
};
