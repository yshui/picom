#include <math.h>
#include "timing_functions.h"

double easeInSine(double t) {
    return 1 - cos((t * M_PI) / 2);
}

double easeOutSine(double t) {
    return sin((t * M_PI) / 2);
}

double easeInOutSine(double t) {
    return -(cos(M_PI * t) - 1) / 2;
}

double easeInCubic(double t) {
    return t * t * t;
}

double easeOutCubic(double t) {
    return 1 - pow(1 - t, 3);
}

double easeInOutCubic(double t) {
    return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
}

double easeInQuint(double t) {
    return t * t * t * t * t;
}

double easeOutQuint(double t) {
    return 1 - pow(1 - t, 5);
}

double easeInOutQuint(double t) {
    return t < 0.5 ? 16 * t * t * t * t * t : 1 - pow(-2 * t + 2, 5) / 2;
}

double easeInCirc(double t) {
    return 1 - sqrt(1 - pow(t, 2));
}

double easeOutCirc(double t) {
    return sqrt(1 - pow(t - 1, 2));
}

double easeInOutCirc(double t) {
    return t < 0.5
    ? (1 - sqrt(1 - pow(2 * t, 2))) / 2
    : (sqrt(1 - pow(-2 * t + 2, 2)) + 1) / 2;
}

double easeInElastic(double t) {
    double c4 = (2 * M_PI) / 3;

    return t == 0
    ? 0
    : t == 1
    ? 1
    : -pow(2, 10 * t - 10) * sin((t * 10 - 10.75) * c4);
}

double easeOutElastic(double t) {
    double c4 = (2 * M_PI) / 3;

    return t == 0
    ? 0
    : t == 1
    ? 1
    : pow(2, -10 * t) * sin((t * 10 - 0.75) * c4) + 1;
}

double easeInOutElastic(double t) {
    double c5 = (2 * M_PI) / 4.5;

    return t == 0
    ? 0
    : t == 1
    ? 1
    : t < 0.5
    ? -(pow(2, 20 * t - 10) * sin((20 * t - 11.125) * c5)) / 2
    : (pow(2, -20 * t + 10) * sin((20 * t - 11.125) * c5)) / 2 + 1;
}

double easeInQuad(double t) {
    return t * t;
}

double easeOutQuad(double t) {
    return 1 - (1 - t) * (1 - t);
}

double easeInOutQuad(double t) {
    return t < 0.5 ? 2 * t * t : 1 - pow(-2 * t + 2, 2) / 2;
}

double easeInQuart(double t) {
    return t * t * t * t;
}

double easeOutQuart(double t) {
    return 1 - pow(1 - t, 4);
}

double easeInOutQuart(double t) {
    return t < 0.5 ? 8 * t * t * t * t : 1 - pow(-2 * t + 2, 4) / 2;
}

double easeInEtpo(double t) {
    return t == 0 ? 0 : pow(2, 10 * t - 10);
}

double easeOutEtpo(double t) {
    return t == 1 ? 1 : 1 - pow(2, -10 * t);
}

double easeInOutEtpo(double t) {
    return t == 0
    ? 0
    : t == 1
    ? 1
    : t < 0.5 ? pow(2, 20 * t - 10) / 2
    : (2 - pow(2, -20 * t + 10)) / 2;
}

double easeInBack(double t) {
    double c1 = 1.70158;
    double c3 = c1 + 1;

    return c3 * t * t * t - c1 * t * t;
}

double easeOutBack(double t) {
    double c1 = 1.70158;
    double c3 = c1 + 1;

    return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2);
}

double easeInOutBack(double t) {
    double c1 = 1.70158;
    double c2 = c1 * 1.525;

    return t < 0.5
    ? (pow(2 * t, 2) * ((c2 + 1) * 2 * t - c2)) / 2
    : (pow(2 * t - 2, 2) * ((c2 + 1) * (t * 2 - 2) + c2) + 2) / 2;
}

double easeInBounce(double t) {
    return 1 - easeOutBounce(1 - t);
}

double easeOutBounce(double t) {
    double n1 = 7.5625;
    double d1 = 2.75;

    if (t < 1 / d1) {
        return n1 * t * t;
    } else if (t < 2 / d1) {
        return n1 * (t -= 1.5 / d1) * t + 0.75;
    } else if (t < 2.5 / d1) {
        return n1 * (t -= 2.25 / d1) * t + 0.9375;
    } else {
        return n1 * (t -= 2.625 / d1) * t + 0.984375;
    }
}

double easeInOutBounce(double t) {
    return t < 0.5
    ? (1 - easeOutBounce(1 - 2 * t)) / 2
    : (1 + easeOutBounce(2 * t - 1)) / 2;
}