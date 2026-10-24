// SPDX-License-Identifier: MIT
#pragma once

// VU1's elementary function unit (EFU), for compiled microprograms. The
// series are the ones the EFU is documented to evaluate.

#include <cmath>

namespace tgr::hw::vu
{

inline double efuReciprocal(double value)
{
    return value != 0 ? 1.0 / value : 3.4028234663852886e38;
}

// arctan by the EFU's series on (x-1)/(x+1), plus pi/4.
inline double efuAtan(double value)
{
    static constexpr double kCoefficients[] = {0.999999344348907,  -0.333298563957214, 0.199465364217758,
                                               -0.13085337519646,  0.096420042216778,  -0.055909886956215,
                                               0.021861229091883,  -0.004054057877511};
    const double t = (value - 1.0) / (value + 1.0);
    const double squared = t * t;
    double polynomial = kCoefficients[7];
    for (int index = 6; index >= 0; --index)
        polynomial = kCoefficients[index] + squared * polynomial;
    return 0.785398185253143 + t * polynomial;
}

inline double efuAtanRatio(double numerator, double denominator)
{
    return denominator != 0 ? efuAtan(numerator / denominator) : 0;
}

inline double efuSin(double value)
{
    static constexpr double kCoefficients[] = {1.0, -0.166666567325592, 0.008333025500178, -0.000198074136279,
                                               0.000002601886990};
    const double squared = value * value;
    double polynomial = kCoefficients[4];
    for (int index = 3; index >= 0; --index)
        polynomial = kCoefficients[index] + squared * polynomial;
    return value * polynomial;
}

inline double efuExp(double value)
{
    static constexpr double kCoefficients[] = {0.249998688697815, 0.031257584691048, 0.002591371303424,
                                               0.000171562001924, 0.000005430199963, 0.000000690600018};
    double polynomial = kCoefficients[5];
    for (int index = 4; index >= 0; --index)
        polynomial = kCoefficients[index] + value * polynomial;
    polynomial = 1.0 + value * polynomial;
    polynomial *= polynomial;
    polynomial *= polynomial;
    return efuReciprocal(polynomial);
}

} // namespace tgr::hw::vu
