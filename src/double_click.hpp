#pragma once

#include <cmath>
#include <cstdint>

namespace wfa {

// Recognises double-clicks: two clicks on the same target (and part of it)
// within a short time and distance.
class DoubleClick {
public:
    static constexpr std::uint32_t interval_ms = 500;
    static constexpr double max_distance = 4;

    // Returns true when this click completes a double-click. A completed
    // double-click does not start another one.
    bool click(const void* target, int part, std::uint32_t time_msec, double x, double y)
    {
        const bool double_click = target && target == m_target && part == m_part
            && time_msec - m_time <= interval_ms && std::abs(x - m_x) <= max_distance
            && std::abs(y - m_y) <= max_distance;
        if (double_click) {
            m_target = nullptr;
        } else {
            m_target = target;
            m_part = part;
            m_time = time_msec;
            m_x = x;
            m_y = y;
        }
        return double_click;
    }

    void reset() { m_target = nullptr; }

private:
    const void* m_target = nullptr;
    int m_part = 0;
    std::uint32_t m_time = 0;
    double m_x = 0;
    double m_y = 0;
};

} // namespace wfa
