// haze.slang — distance haze between the camera and the scene.
//
// Pure math: no global state, no imports. Both renderers call it on the
// radiance of a camera ray, with the point where that ray ends, so the same
// scene renders the same haze in Hyperion and in Theia.
//
// The medium is the scene's `[haze]` section: an extinction coefficient that
// falls off exponentially with height,
//
//     sigma(h) = density * exp(-h / height)                 (h = height above 0, metres)
//
// A camera ray that ends at the distance d loses the fraction exp(-tau) of
// the radiance coming from there, with tau the optical depth along the ray,
// and gains light that the medium scatters toward the camera in its place.
// The in-scattered light is approximated by the sky behind the end of the ray,
// read from a strongly blurred environment map: far objects then fade into the
// sky that lies behind them, and a sight line toward the sun glows. The sky
// itself is the radiance at infinity and is left unchanged.
//
// Shadow rays and secondary rays do not pass through the medium: the haze
// acts on what the camera sees, once, per camera ray.

struct Haze {
    float density; ///< extinction per metre at height zero
    float height;  ///< metres above height zero at which the extinction has fallen to 1/e
};

/// False for the default (no haze) and for a haze whose keys are not usable.
bool hazeActive(Haze haze) {
    return haze.density > 0.0 && haze.height > 0.0;
}

/// Optical depth along the segment a -> b (Y is up).
float hazeOpticalDepth(Haze haze, float3 a, float3 b) {
    if (!hazeActive(haze)) {
        return 0.0;
    }
    const float3 segment = b - a;
    const float length_ = length(segment);
    if (length_ < 1.0e-6) {
        return 0.0;
    }
    // ∫ sigma(h(s)) ds over s in [0, length], with h(s) = a.y + (b.y - a.y) * s / length:
    //   density * length * exp(-a.y / height) * (1 - exp(-x)) / x,   x = (b.y - a.y) / height
    // The last factor tends to 1 for a horizontal segment and to 1 / x for a long
    // descent, so the expression stays accurate for every elevation.
    const float x = (b.y - a.y) / haze.height;
    const float factor = (abs(x) > 1.0e-4) ? ((1.0 - exp(-x)) / x) : (1.0 - 0.5 * x);
    return haze.density * length_ * exp(-a.y / haze.height) * factor;
}

/// The radiance the camera sees through the medium: what is left of the
/// radiance from the end of the ray, plus the light scattered into its place.
/// `sky` is the blurred radiance of the sky in the direction of the ray.
float3 hazeComposite(float3 radiance, float opticalDepth, float3 sky) {
    const float transmittance = exp(-max(opticalDepth, 0.0));
    return transmittance * radiance + (1.0 - transmittance) * sky;
}

/// The share of a radiance that reaches the camera through the medium. For a
/// term that is added to a pixel whose other terms were already composited.
float hazeTransmittance(float opticalDepth) {
    return exp(-max(opticalDepth, 0.0));
}

/// Level of the environment map's mip chain at which one texel spans about
/// six degrees: the blur of the in-scattered sky. A map without a mip chain
/// clamps to its only level, and the haze then follows the sky's detail.
static const float kHazeSkyLod = 6.0;
