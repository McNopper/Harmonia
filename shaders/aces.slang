// Full ACES 1.0 SDR Video (RRT.a1.0.3 + RGBmonitor ODT.a1.0.3).
// Reference: aces-core v1.3 / OCIO ACES 1.0 - SDR Video. Not the fitted ACES curve.

static const float kAcesHalfMin = 5.960464477539063e-8;
static const float kAcesHalfMax = 65504.0;
static const float kAcesPi = 3.14159265358979323846;
static const float3 kAp1Rgb2Y = float3(0.2722287168, 0.6740817658, 0.0536895174);

static const float3x3 kAcesAp0ToAp1 = float3x3(
    float3( 1.4514393161, -0.2365107469, -0.2149285693),
    float3(-0.0765537734,  1.1762296998, -0.0996759264),
    float3( 0.0083161484, -0.0060324498,  0.9977163014));
static const float3x3 kAcesAp1ToAp0 = float3x3(
    float3( 0.6954522414,  0.1406786965,  0.1638690622),
    float3( 0.0447945634,  0.8596711185,  0.0955343182),
    float3(-0.0055258826,  0.0040252103,  1.0015006723));
static const float3x3 kAcesAp1ToXyz = float3x3(
    float3( 0.6624541811,  0.1340042065,  0.1561876870),
    float3( 0.2722287168,  0.6740817658,  0.0536895174),
    float3(-0.0055746495,  0.0040607335,  1.0103391003));
static const float3x3 kAcesXyzToAp1 = float3x3(
    float3( 1.6410233800, -0.3248032900, -0.2364247000),
    float3(-0.6636628600,  1.6153315900,  0.0167563500),
    float3( 0.0117218900, -0.0082844400,  0.9883948600));
static const float3x3 kAcesD60ToD65 = float3x3(
    float3( 0.9872240087, -0.0061132286,  0.0159532883),
    float3(-0.0075983718,  1.0018614847,  0.0053300358),
    float3( 0.0030725771, -0.0050959615,  1.0816806031));
static const float3x3 kAcesXyzD65ToRec709 = float3x3(
    float3( 3.2409699419, -1.5373831776, -0.4986107603),
    float3(-0.9692436363,  1.8759675015,  0.0415550574),
    float3( 0.0556300797, -0.2039769589,  1.0569715142));
static const float3x3 kAcesRec709ToAp1 = float3x3(
    float3(0.6130974024, 0.3395231462, 0.0473794514),
    float3(0.0701937225, 0.9163538791, 0.0134523985),
    float3(0.0206155929, 0.1095697729, 0.8698146342));
static const float3x3 kAcesRec2020ToAp1 = float3x3(
    float3(0.9748949779, 0.0195991086, 0.0055059134),
    float3(0.0021795628, 0.9955354689, 0.0022849683),
    float3(0.0047972397, 0.0245320166, 0.9706707437));

static const float kAcesC5Low[10] = {-4.0, -4.0, -3.1573765773, -0.4852499958, 1.8477324706, 1.8477324706, 0, 0, 0, 0};
static const float kAcesC5High[10] = {-0.7185482425, 2.0810307172, 3.6681241237, 4.0, 4.0, 4.0, 0, 0, 0, 0};
static const float kAcesC9Low[10] = {-1.6989700043, -1.6989700043, -1.4779, -1.2291, -0.8648, -0.448, 0.00518, 0.4511080334, 0.9113744414, 0.9113744414};
static const float kAcesC9High[10] = {0.5154386965, 0.8470437783, 1.1358, 1.3802, 1.5197, 1.5985, 1.6467, 1.6746091357, 1.6878733390, 1.6878733390};

float acesLog10(float x) { return log(x) / log(10.0); }
float acesPow10(float x) { return pow(10.0, x); }

float acesSegmentedSpline(float x, float coefficientsLow[10], float coefficientsHigh[10],
                          int knotsLow, int knotsHigh, float minX, float minY,
                          float midX, float midY, float maxX, float maxY,
                          float slopeLow, float slopeHigh) {
    float logx = acesLog10(max(x, kAcesHalfMin));
    if (logx <= acesLog10(minX)) {
        return acesPow10(logx * slopeLow + (acesLog10(minY) - slopeLow * acesLog10(minX)));
    }
    bool low = logx < acesLog10(midX);
    bool middle = !low && logx < acesLog10(maxX);
    if (low || middle) {
        float startX = low ? minX : midX;
        float endX = low ? midX : maxX;
        int knots = low ? knotsLow : knotsHigh;
        float knot = (knots - 1.0) * (logx - acesLog10(startX)) / (acesLog10(endX) - acesLog10(startX));
        int j = int(knot);
        float t = knot - j;
        float c0 = low ? coefficientsLow[j] : coefficientsHigh[j];
        float c1 = low ? coefficientsLow[j + 1] : coefficientsHigh[j + 1];
        float c2 = low ? coefficientsLow[j + 2] : coefficientsHigh[j + 2];
        float a = 0.5 * c0 - c1 + 0.5 * c2;
        float b = -c0 + c1;
        float c = 0.5 * c0 + 0.5 * c1;
        return acesPow10((a * t + b) * t + c);
    }
    return acesPow10(logx * slopeHigh + (acesLog10(maxY) - slopeHigh * acesLog10(maxX)));
}

float acesC5(float x) {
    return acesSegmentedSpline(x, kAcesC5Low, kAcesC5High, 4, 4,
        0.18 * exp2(-15.0), 0.0001, 0.18, 4.8, 0.18 * exp2(18.0), 10000.0, 0.0, 0.0);
}
float acesC9(float x) {
    return acesSegmentedSpline(x, kAcesC9Low, kAcesC9High, 8, 8,
        acesC5(0.18 * exp2(-6.5)), 0.02, acesC5(0.18), 4.8,
        acesC5(0.18 * exp2(6.5)), 48.0, 0.0, 0.04);
}

float acesSaturation(float3 rgb) {
    float maximum = max(rgb.x, max(rgb.y, rgb.z));
    float minimum = min(rgb.x, min(rgb.y, rgb.z));
    return (max(maximum, 1e-10) - max(minimum, 1e-10)) / max(maximum, 1e-2);
}
float acesRgbToYc(float3 rgb) {
    float chroma2 = rgb.z * (rgb.z - rgb.y) + rgb.y * (rgb.y - rgb.x) + rgb.x * (rgb.x - rgb.z);
    return (rgb.x + rgb.y + rgb.z + 1.75 * sqrt(max(chroma2, 0.0))) / 3.0;
}
float acesSigmoid(float x) {
    float t = max(1.0 - abs(x / 2.0), 0.0);
    return (1.0 + (x < 0.0 ? -1.0 : 1.0) * (1.0 - t * t)) / 2.0;
}
float acesGlow(float yc, float gain, float mid) {
    if (yc <= (2.0 / 3.0) * mid) return gain;
    if (yc >= 2.0 * mid) return 0.0;
    return gain * (mid / yc - 0.5);
}
float acesCubicBasis(float x, float width) {
    const float m[4][4] = {{-1.0/6, 3.0/6, -3.0/6, 1.0/6},
                           {3.0/6, -6.0/6, 3.0/6, 0.0},
                           {-3.0/6, 0.0, 3.0/6, 0.0},
                           {1.0/6, 4.0/6, 1.0/6, 0.0}};
    if (!(x > -width / 2.0 && x < width / 2.0)) return 0.0;
    float knot = (x + width / 2.0) * 4.0 / width;
    int j = int(knot);
    float t = knot - j;
    if (j < 0 || j > 3) return 0.0;
    float v[4] = {t*t*t, t*t, t, 1.0};
    int column = 3 - j;
    float y = 0.0;
    for (int row = 0; row < 4; ++row) y += v[row] * m[row][column];
    return y * 1.5;
}
float acesCenteredHue(float3 rgb) {
    if (rgb.x == rgb.y && rgb.y == rgb.z) return 0.0;
    float hue = (180.0 / kAcesPi) * atan2(sqrt(3.0) * (rgb.y - rgb.z), 2.0 * rgb.x - rgb.y - rgb.z);
    if (hue < 0.0) hue += 360.0;
    if (hue > 180.0) hue -= 360.0;
    return hue;
}
float3 acesSaturateAp1(float3 rgb, float saturation) {
    float y = dot(rgb, kAp1Rgb2Y);
    return rgb * saturation + y.xxx * (1.0 - saturation);
}
float3 acesRrt(float3 ap0) {
    float saturation = acesSaturation(ap0);
    float s = acesSigmoid((saturation - 0.4) / 0.2);
    float3 rgb = ap0 * (1.0 + acesGlow(acesRgbToYc(ap0), 0.05 * s, 0.08));
    float hueWeight = acesCubicBasis(acesCenteredHue(rgb), 135.0);
    rgb.x += hueWeight * saturation * (0.03 - rgb.x) * (1.0 - 0.82);
    rgb = max(rgb, 0.0.xxx);
    float3 ap1 = mul(kAcesAp0ToAp1, rgb);
    ap1 = clamp(ap1, 0.0.xxx, kAcesHalfMax.xxx);
    ap1 = acesSaturateAp1(ap1, 0.96);
    ap1 = float3(acesC5(ap1.x), acesC5(ap1.y), acesC5(ap1.z));
    return mul(kAcesAp1ToAp0, ap1);
}
float3 acesDarkToDim(float3 linearCv) {
    float3 xyz = mul(kAcesAp1ToXyz, linearCv);
    float divisor = xyz.x + xyz.y + xyz.z;
    if (divisor == 0.0) divisor = 1e-10;
    float x = xyz.x / divisor;
    float y = xyz.y / divisor;
    float luminance = pow(max(xyz.y, 0.0), 0.9811);
    float denominator = max(y, 1e-10);
    float3 adjusted = float3(x * luminance / denominator, luminance,
        (1.0 - x - y) * luminance / denominator);
    return mul(kAcesXyzToAp1, adjusted);
}
float3 acesOdtToXyzD65(float3 ocesAp0) {
    float3 ap1 = mul(kAcesAp0ToAp1, ocesAp0);
    ap1 = float3(acesC9(ap1.x), acesC9(ap1.y), acesC9(ap1.z));
    ap1 = (ap1 - 0.02.xxx) / (48.0 - 0.02);
    ap1 = acesDarkToDim(ap1);
    ap1 = acesSaturateAp1(ap1, 0.93);
    return mul(kAcesD60ToD65, mul(kAcesAp1ToXyz, ap1));
}
float3 acesOdtToLinearRec709(float3 ocesAp0) {
    return saturate(mul(kAcesXyzD65ToRec709, acesOdtToXyzD65(ocesAp0)));
}
float acesSrgbEncode(float x) {
    const float gamma = 2.4, offset = 0.055;
    float breakpoint = pow(offset * gamma / ((gamma - 1.0) * (1.0 + offset)), gamma);
    float slope = pow((gamma - 1.0) / offset, gamma - 1.0) * pow((1.0 + offset) / gamma, gamma);
    return x >= breakpoint ? (1.0 + offset) * pow(x, 1.0 / gamma) - offset : x * slope;
}
float3 acesWorkingToAp0(float3 rgb, uint workingSpace) {
    float3 ap1 = workingSpace == 1u ? mul(kAcesRec709ToAp1, rgb)
               : workingSpace == 2u ? rgb : mul(kAcesRec2020ToAp1, rgb);
    return mul(kAcesAp1ToAp0, ap1);
}
float3 aces10SdrVideoToSrgbFromWorking(float3 rgb, uint workingSpace) {
    float3 oces = acesRrt(acesWorkingToAp0(rgb, workingSpace));
    float3 linear = acesOdtToLinearRec709(oces);
    return float3(acesSrgbEncode(linear.x), acesSrgbEncode(linear.y), acesSrgbEncode(linear.z));
}
float3 aces10SdrVideoToLinearFromWorking(float3 rgb, uint workingSpace) {
    return acesOdtToLinearRec709(acesRrt(acesWorkingToAp0(rgb, workingSpace)));
}
