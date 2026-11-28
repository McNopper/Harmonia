#include "harmonia/utils/Aces.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace harmonia::utils::Aces {
namespace {

constexpr float kHalfMin = 5.960464477539063e-8F; // 2^-24, CTL HALF_MIN
constexpr float kHalfMax = 65504.0F;
constexpr float kPi = 3.14159265358979323846F;

// Column-vector matrices, derived from ACESlib.Transform_Common/Utilities_Color.
// clang-format off
const sm::float3x3 kAp0ToAp1{
    { 1.4514393161F, -0.2365107469F, -0.2149285693F},
    {-0.0765537734F,  1.1762296998F, -0.0996759264F},
    { 0.0083161484F, -0.0060324498F,  0.9977163014F}};
const sm::float3x3 kAp1ToAp0{
    { 0.6954522414F,  0.1406786965F,  0.1638690622F},
    { 0.0447945634F,  0.8596711185F,  0.0955343182F},
    {-0.0055258826F,  0.0040252103F,  1.0015006723F}};
const sm::float3x3 kAp1ToXyz{
    { 0.6624541811F,  0.1340042065F,  0.1561876870F},
    { 0.2722287168F,  0.6740817658F,  0.0536895174F},
    {-0.0055746495F,  0.0040607335F,  1.0103391003F}};
const sm::float3x3 kXyzToAp1{
    { 1.6410233800F, -0.3248032900F, -0.2364247000F},
    {-0.6636628600F,  1.6153315900F,  0.0167563500F},
    { 0.0117218900F, -0.0082844400F,  0.9883948600F}};
const sm::float3x3 kD60ToD65{
    { 0.9872240087F, -0.0061132286F,  0.0159532883F},
    {-0.0075983718F,  1.0018614847F,  0.0053300358F},
    { 0.0030725771F, -0.0050959615F,  1.0816806031F}};
const sm::float3x3 kXyzD65ToRec709{
    { 3.2409699419F, -1.5373831776F, -0.4986107603F},
    {-0.9692436363F,  1.8759675015F,  0.0415550574F},
    { 0.0556300797F, -0.2039769589F,  1.0569715142F}};
// clang-format on

constexpr sm::float3 kAp1Rgb2Y{0.2722287168F, 0.6740817658F, 0.0536895174F};
constexpr float kRrtSat = 0.96F;
constexpr float kOdtSat = 0.93F;
constexpr float kCinemaWhite = 48.0F;
constexpr float kCinemaBlack = 0.02F;
constexpr float kDimSurroundGamma = 0.9811F;

struct SplineParams {
    const float* low;
    const float* high;
    int knotsLow;
    int knotsHigh;
    float minX, minY, midX, midY, maxX, maxY;
    float slopeLow, slopeHigh;
};

float log10f(float x) {
    return std::log10(x);
}
float pow10f(float x) {
    return std::pow(10.0F, x);
}

float segmentedSpline(float x, const SplineParams& p) {
    const float logx = log10f(std::max(x, kHalfMin));
    float logy;
    if (logx <= log10f(p.minX)) {
        logy = logx * p.slopeLow + (log10f(p.minY) - p.slopeLow * log10f(p.minX));
        return pow10f(logy);
    }
    const bool low = logx < log10f(p.midX);
    const bool middle = !low && logx < log10f(p.maxX);
    if (low || middle) {
        const float startX = low ? p.minX : p.midX;
        const float endX = low ? p.midX : p.maxX;
        const int knots = low ? p.knotsLow : p.knotsHigh;
        const float* coefficients = low ? p.low : p.high;
        const float knot = (knots - 1.0F) * (logx - log10f(startX)) / (log10f(endX) - log10f(startX));
        const int j = static_cast<int>(knot);
        const float t = knot - j;
        const float c0 = coefficients[j], c1 = coefficients[j + 1], c2 = coefficients[j + 2];
        // cf * M, CTL row-vector convention; M = {{.5,-1,.5},{-1,1,.5},{.5,0,0}}.
        const float a = 0.5F * c0 - c1 + 0.5F * c2;
        const float b = -c0 + c1;
        const float c = 0.5F * c0 + 0.5F * c1;
        logy = (a * t + b) * t + c;
        return pow10f(logy);
    }
    logy = logx * p.slopeHigh + (log10f(p.maxY) - p.slopeHigh * log10f(p.maxX));
    return pow10f(logy);
}

constexpr float kC5Low[] = {-4.0F, -4.0F, -3.1573765773F, -0.4852499958F, 1.8477324706F, 1.8477324706F};
constexpr float kC5High[] = {-0.7185482425F, 2.0810307172F, 3.6681241237F, 4.0F, 4.0F, 4.0F};
constexpr float kC9Low[] = {-1.6989700043F,
                            -1.6989700043F,
                            -1.4779F,
                            -1.2291F,
                            -0.8648F,
                            -0.448F,
                            0.00518F,
                            0.4511080334F,
                            0.9113744414F,
                            0.9113744414F};
constexpr float kC9High[] = {0.5154386965F,
                             0.8470437783F,
                             1.1358F,
                             1.3802F,
                             1.5197F,
                             1.5985F,
                             1.6467F,
                             1.6746091357F,
                             1.6878733390F,
                             1.6878733390F};

float rgbSaturation(sm::float3 rgb) {
    const float maximum = std::max({rgb.x, rgb.y, rgb.z});
    const float minimum = std::min({rgb.x, rgb.y, rgb.z});
    return (std::max(maximum, 1.0e-10F) - std::max(minimum, 1.0e-10F)) / std::max(maximum, 1.0e-2F);
}

float rgbToYc(sm::float3 rgb) {
    const float chroma2 = rgb.z * (rgb.z - rgb.y) + rgb.y * (rgb.y - rgb.x) + rgb.x * (rgb.x - rgb.z);
    return (rgb.x + rgb.y + rgb.z + 1.75F * std::sqrt(std::max(chroma2, 0.0F))) / 3.0F;
}

float sigmoidShaper(float x) {
    const float t = std::max(1.0F - std::abs(x / 2.0F), 0.0F);
    return (1.0F + (x < 0.0F ? -1.0F : 1.0F) * (1.0F - t * t)) / 2.0F;
}

float glowForward(float yc, float gain, float mid) {
    if (yc <= (2.0F / 3.0F) * mid)
        return gain;
    if (yc >= 2.0F * mid)
        return 0.0F;
    return gain * (mid / yc - 0.5F);
}

float cubicBasisShaper(float x, float width) {
    constexpr float m[4][4] = {{-1.0F / 6, 3.0F / 6, -3.0F / 6, 1.0F / 6},
                               {3.0F / 6, -6.0F / 6, 3.0F / 6, 0.0F},
                               {-3.0F / 6, 0.0F, 3.0F / 6, 0.0F},
                               {1.0F / 6, 4.0F / 6, 1.0F / 6, 0.0F}};
    const float first = -width / 2.0F, last = width / 2.0F;
    if (!(x > first && x < last))
        return 0.0F;
    const float knot = (x - first) * 4.0F / width;
    const int j = static_cast<int>(knot);
    const float t = knot - j;
    const float v[4] = {t * t * t, t * t, t, 1.0F};
    if (j < 0 || j > 3)
        return 0.0F;
    const int column = 3 - j;
    float y = 0.0F;
    for (int row = 0; row < 4; ++row)
        y += v[row] * m[row][column];
    return y * 1.5F;
}

float centeredHue(sm::float3 rgb) {
    if (rgb.x == rgb.y && rgb.y == rgb.z)
        return 0.0F; // CTL's undefined neutral hue produces zero red weight.
    float hue = (180.0F / kPi) * std::atan2(std::sqrt(3.0F) * (rgb.y - rgb.z), 2.0F * rgb.x - rgb.y - rgb.z);
    if (hue < 0.0F)
        hue += 360.0F;
    if (hue > 180.0F)
        hue -= 360.0F;
    return hue;
}

sm::float3 saturateAp1(sm::float3 rgb, float saturation) {
    const float y = sm::dot(rgb, kAp1Rgb2Y);
    return rgb * saturation + sm::float3(y) * (1.0F - saturation);
}

sm::float3 darkToDim(sm::float3 linearCv) {
    const sm::float3 xyz = kAp1ToXyz * linearCv;
    float divisor = xyz.x + xyz.y + xyz.z;
    if (divisor == 0.0F)
        divisor = 1.0e-10F;
    const float x = xyz.x / divisor;
    const float y = xyz.y / divisor;
    const float luminance = std::pow(std::max(xyz.y, 0.0F), kDimSurroundGamma);
    const float denominator = std::max(y, 1.0e-10F);
    const sm::float3 adjusted{x * luminance / denominator, luminance, (1.0F - x - y) * luminance / denominator};
    return kXyzToAp1 * adjusted;
}

float srgbEncode(float x) {
    // CTL moncurve_r(2.4,.055), equivalent to IEC 61966-2-1 at the common breakpoint.
    constexpr float gamma = 2.4F, offset = 0.055F;
    const float breakpoint = std::pow(offset * gamma / ((gamma - 1.0F) * (1.0F + offset)), gamma);
    const float slope = std::pow((gamma - 1.0F) / offset, gamma - 1.0F) * std::pow((1.0F + offset) / gamma, gamma);
    return x >= breakpoint ? (1.0F + offset) * std::pow(x, 1.0F / gamma) - offset : x * slope;
}

} // namespace

float segmentedSplineC5(float x) noexcept {
    const SplineParams p{kC5Low,
                         kC5High,
                         4,
                         4,
                         0.18F * std::exp2(-15.0F),
                         0.0001F,
                         0.18F,
                         4.8F,
                         0.18F * std::exp2(18.0F),
                         10000.0F,
                         0.0F,
                         0.0F};
    return segmentedSpline(x, p);
}

float segmentedSplineC9Odt48(float x) noexcept {
    const SplineParams p{kC9Low,
                         kC9High,
                         8,
                         8,
                         segmentedSplineC5(0.18F * std::exp2(-6.5F)),
                         0.02F,
                         segmentedSplineC5(0.18F),
                         4.8F,
                         segmentedSplineC5(0.18F * std::exp2(6.5F)),
                         48.0F,
                         0.0F,
                         0.04F};
    return segmentedSpline(x, p);
}

sm::float3 acesCgToAces2065(sm::float3 ap1) noexcept {
    return kAp1ToAp0 * ap1;
}
sm::float3 aces2065ToAcesCg(sm::float3 ap0) noexcept {
    return kAp0ToAp1 * ap0;
}

sm::float3 rrt10AcesToOces(sm::float3 ap0) noexcept {
    const float saturation = rgbSaturation(ap0);
    const float s = sigmoidShaper((saturation - 0.4F) / 0.2F);
    const float glow = 1.0F + glowForward(rgbToYc(ap0), 0.05F * s, 0.08F);
    sm::float3 aces = ap0 * glow;
    const float hueWeight = cubicBasisShaper(centeredHue(aces), 135.0F);
    aces.x += hueWeight * saturation * (0.03F - aces.x) * (1.0F - 0.82F);
    aces = sm::max(aces, sm::float3(0.0F));
    sm::float3 ap1 = kAp0ToAp1 * aces;
    ap1 = sm::clamp(ap1, sm::float3(0.0F), sm::float3(kHalfMax));
    ap1 = saturateAp1(ap1, kRrtSat);
    ap1 = {segmentedSplineC5(ap1.x), segmentedSplineC5(ap1.y), segmentedSplineC5(ap1.z)};
    return kAp1ToAp0 * ap1;
}

sm::float3 odt10SdrVideoToXyzD65(sm::float3 ocesAp0) noexcept {
    sm::float3 ap1 = kAp0ToAp1 * ocesAp0;
    ap1 = {segmentedSplineC9Odt48(ap1.x), segmentedSplineC9Odt48(ap1.y), segmentedSplineC9Odt48(ap1.z)};
    ap1 = (ap1 - sm::float3(kCinemaBlack)) / (kCinemaWhite - kCinemaBlack);
    ap1 = darkToDim(ap1);
    ap1 = saturateAp1(ap1, kOdtSat);
    return kD60ToD65 * (kAp1ToXyz * ap1);
}

sm::float3 odt10SdrVideoToLinearRec709(sm::float3 ocesAp0) noexcept {
    const sm::float3 rec709 = kXyzD65ToRec709 * odt10SdrVideoToXyzD65(ocesAp0);
    return sm::clamp(rec709, sm::float3(0.0F), sm::float3(1.0F));
}

sm::float3 odt10SdrVideoToSrgb(sm::float3 ocesAp0) noexcept {
    const sm::float3 linear = odt10SdrVideoToLinearRec709(ocesAp0);
    return {srgbEncode(linear.x), srgbEncode(linear.y), srgbEncode(linear.z)};
}

sm::float3 aces10SdrVideoToSrgb(sm::float3 ap0) noexcept {
    return odt10SdrVideoToSrgb(rrt10AcesToOces(ap0));
}

sm::float3 aces10SdrVideoToSrgbFromAcesCg(sm::float3 ap1) noexcept {
    return aces10SdrVideoToSrgb(acesCgToAces2065(ap1));
}

} // namespace harmonia::utils::Aces
