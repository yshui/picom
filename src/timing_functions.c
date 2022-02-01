#include "timing_functions.h"
#include <math.h>

// clang-format off
double ease_in_sine(double t) {
    return 1 - cos((t * M_PI) / 2);
}

double ease_out_sine(double t) {
    return sin((t * M_PI) / 2);
}

double ease_in_out_sine(double t) {
    return -(cos(M_PI * t) - 1) / 2;
}

double ease_in_cubic(double t) {
    return t * t * t;
}

double ease_out_cubic(double t) {
    return 1 - pow(1 - t, 3);
}

double ease_in_out_cubic(double t) {
    return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
}

double ease_in_quint(double t) {
    return t * t * t * t * t;
}

double ease_out_quint(double t) {
    return 1 - pow(1 - t, 5);
}

double ease_in_out_quint(double t) {
    return t < 0.5 ? 16 * t * t * t * t * t : 1 - pow(-2 * t + 2, 5) / 2;
}

double ease_in_circ(double t) {
    return 1 - sqrt(1 - pow(t, 2));
}

double ease_out_circ(double t) {
    return sqrt(1 - pow(t - 1, 2));
}

double ease_in_out_circ(double t) {
    return t < 0.5
    ? (1 - sqrt(1 - pow(2 * t, 2))) / 2
    : (sqrt(1 - pow(-2 * t + 2, 2)) + 1) / 2;
}

double ease_in_elastic(double t) {
    double c4 = (2 * M_PI) / 3;

    return t == 0
    ? 0
    : t == 1
    ? 1
    : -pow(2, 10 * t - 10) * sin((t * 10 - 10.75) * c4);
}

double ease_out_elastic(double t) {
    double c4 = (2 * M_PI) / 3;

    return t == 0
    ? 0
    : t == 1
    ? 1
    : pow(2, -10 * t) * sin((t * 10 - 0.75) * c4) + 1;
}

double ease_in_out_elastic(double t) {
    double c5 = (2 * M_PI) / 4.5;

    return t == 0
    ? 0
    : t == 1
    ? 1
    : t < 0.5
    ? -(pow(2, 20 * t - 10) * sin((20 * t - 11.125) * c5)) / 2
    : (pow(2, -20 * t + 10) * sin((20 * t - 11.125) * c5)) / 2 + 1;
}

double ease_in_quad(double t) {
    return t * t;
}

double ease_out_quad(double t) {
    return 1 - (1 - t) * (1 - t);
}

double ease_in_out_quad(double t) {
    return t < 0.5 ? 2 * t * t : 1 - pow(-2 * t + 2, 2) / 2;
}

double ease_in_quart(double t) {
    return t * t * t * t;
}

double ease_out_quart(double t) {
    return 1 - pow(1 - t, 4);
}

double ease_in_out_quart(double t) {
    return t < 0.5 ? 8 * t * t * t * t : 1 - pow(-2 * t + 2, 4) / 2;
}

double ease_in_etpo(double t) {
    return t == 0 ? 0 : pow(2, 10 * t - 10);
}

double ease_out_etpo(double t) {
    return t == 1 ? 1 : 1 - pow(2, -10 * t);
}

double ease_in_out_etpo(double t) {
    return t == 0
    ? 0
    : t == 1
    ? 1
    : t < 0.5 ? pow(2, 20 * t - 10) / 2
    : (2 - pow(2, -20 * t + 10)) / 2;
}

double ease_in_back(double t) {
    double c1 = 1.70158;
    double c3 = c1 + 1;

    return c3 * t * t * t - c1 * t * t;
}

double ease_out_back(double t) {
    double c1 = 1.70158;
    double c3 = c1 + 1;

    return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2);
}

double ease_in_out_back(double t) {
    double c1 = 1.70158;
    double c2 = c1 * 1.525;

    return t < 0.5
    ? (pow(2 * t, 2) * ((c2 + 1) * 2 * t - c2)) / 2
    : (pow(2 * t - 2, 2) * ((c2 + 1) * (t * 2 - 2) + c2) + 2) / 2;
}

double ease_in_bounce(double t) {
    return 1 - ease_out_bounce(1 - t);
}

double ease_out_bounce(double t) {
    double n1 = 7.5625;
    double d1 = 2.75;

    if (t < 1 / d1) {
        return n1 * t * t;
    } else if (t < 2 / d1) {
        t -= 1.5 / d1;
        return n1 * t * t + 0.75;
    } else if (t < 2.5 / d1) {
        t -= 2.25 / d1;
        return n1 * t * t + 0.9375;
    } else {
        t -= 2.625 / d1;
        return n1 * t * t + 0.984375;
    }
}

double ease_in_out_bounce(double t) {
    return t < 0.5
    ? (1 - ease_out_bounce(1 - 2 * t)) / 2
    : (1 + ease_out_bounce(2 * t - 1)) / 2;
}
// clang-format on