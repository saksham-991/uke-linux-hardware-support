// SPDX-License-Identifier: Apache-2.0

#include "tip-pressure.hpp"

#include <cassert>

using namespace std::chrono_literals;

int main()
{
    TipPressure pressure;
    const auto start = TipPressure::Stamp{} + 1s;
    assert(!pressure.deadline());
    assert(!pressure.sample(600, start));
    assert(pressure.for_frame(start + 20ms, start + 20ms) == 600);
    assert(!pressure.expire(start + 37ms));
    assert(pressure.expire(start + 45ms));
    assert(!pressure.expire(start + 46ms));
    assert(!pressure.deadline());

    const auto lift = start + 65ms;
    assert(!pressure.sample(390, start + 60ms));
    assert(!pressure.sample(180, lift - 1ms));
    assert(pressure.for_frame(lift - 1ms, lift - 1ms) == 180);
    assert(pressure.sample(148, lift));
    assert(pressure.for_frame(lift, lift) == 0);
    assert(!pressure.sample(180, lift + 1ms));
    assert(!pressure.deadline());
    assert(!pressure.sample(200, lift + 4ms));
    assert(pressure.for_frame(lift - 1ms, lift + 5ms) == 0);
    assert(pressure.for_frame(lift + 5ms, lift + 5ms) == 200);
    assert(pressure.deadline() == lift + 49ms);
    assert(pressure.for_frame(lift + 50ms, lift + 50ms) == 0);
    assert(pressure.sample(0, lift + 30ms));
    assert(!pressure.deadline());
}
