// Supported transport domain, shared by SMS proposal and ordinary-PT ownership.
// This is deliberately not equivalent to BSDFSample.isDelta (dispersion and
// medium-entry conventions also use that flag). Never classify by solve success.
import common;
import math;
import bsdf;
__exported import sms_solver;

bool smsUntexturedOpaqueCaster(GpuMaterial mat) {
    return mat.opacityFlagsPad.x == 1.0 && all(mat.textureIndices == kNoTexture.xxxx) &&
        all(mat.textureIndices2 == kNoTexture.xxxx);
}

bool smsNonEmissive(GpuMaterial mat) {
    return mat.emissionColorLum.w <= 0.0 || all(mat.emissionColorLum.xyz <= 0.0.xxx);
}

bool smsEmitterMaterial(GpuMaterial mat) {
    return smsUntexturedOpaqueCaster(mat) && mat.coatColorWeight.w == 0.0 && !smsNonEmissive(mat);
}

float smsEmitterSelectionMass(float current, float previous) {
    if (!isfinite(current) || !isfinite(previous) || previous < 0.0 || current > 1.0 || current <= previous) return 0.0;
    return current - previous;
}

float smsEmitterArea(GpuEmissiveTriangle light) {
    return light.kind_pad.x == kEmissiveTriangle ? light.v0_area.w :
        4.0*kPi*light.v0_area.w*light.v0_area.w;
}

bool smsProposableEmitter(GpuEmissiveTriangle light, GpuMaterial mat, float mass) {
    float area = smsEmitterArea(light);
    float pdf = mass / area;
    return light.kind_pad.x <= kEmissiveSphere && smsEmitterMaterial(mat) &&
        mass > 0.0 && isfinite(mass) && area > 0.0 && isfinite(area) && pdf > 0.0 && isfinite(pdf);
}

bool smsLegacyMirror(GpuMaterial mat) {
    return smsUntexturedOpaqueCaster(mat) && smsNonEmissive(mat) && uint(mat.opacityFlagsPad.y) == 3u;
}

bool smsPureAbsorption(GpuMaterial mat) {
    return mat.transmissionParams.x > 1.0e-4 && all(mat.transmissionScatter.xyz == 0.0.xxx);
}

bool smsSmoothGlass(GpuMaterial mat) {
    return smsUntexturedOpaqueCaster(mat) && smsNonEmissive(mat) && uint(mat.opacityFlagsPad.y) == 2u &&
        mat.specularRoughAnisoIor.x < kSmoothDielectricRoughness && mat.transmissionParams.z <= 0.0 &&
        mat.opacityFlagsPad.w <= 0.5 && mat.baseColorWeight.w >= 1.0e-6 &&
        mat.coatColorWeight.w == 0.0 && mat.fuzzColorWeight.w == 0.0 &&
        (mat.transmissionParams.x <= 1.0e-4 || smsPureAbsorption(mat)) && smsGlassEta(mat) > 1.0;
}

bool smsReflectionCaster(GpuMaterial mat) { return smsLegacyMirror(mat) || smsSmoothGlass(mat); }

bool smsOpaqueReceiver(GpuMaterial mat) {
    return mat.opacityFlagsPad.x == 1.0 && uint(mat.opacityFlagsPad.y) != 3u &&
        uint(mat.opacityFlagsPad.y) != 2u && mat.transmissionColorWeight.w == 0.0 &&
        mat.subsurfaceColorWeight.w == 0.0 && smsNonEmissive(mat);
}

float smsGlassEta(GpuMaterial mat) {
    return openpbrModulatedEta(max(mat.specularRoughAnisoIor.z, 1.01),
        max(mat.specularColorWeight.w, 0.0), max(mat.coatRoughAnisoIorDark.z, 1.01),
        saturate(mat.coatColorWeight.w));
}

float3 smsDeltaResponse(GpuMaterial mat, float3 wo, float3 normal, uint event, float etaTOverI) {
    // Exactly sampleBSDF's pre-lobe zero-event support, not a new optical cutoff.
    bool mediumExit = event == kSmsTransmission && etaTOverI < 1.0 && smsPureAbsorption(mat);
    if (!mediumExit && abs(dot(wo, normal)) <= kBsdfGrazingCosine) return 0.0.xxx;
    if (smsLegacyMirror(mat)) return saturate3(mat.baseColorWeight.xyz + mat.specularColorWeight.xyz);
    if (!smsSmoothGlass(mat)) return 0.0.xxx;
    float eta = event == kSmsReflection ? smsGlassEta(mat) : etaTOverI;
    float F = fresnelDielectric(abs(dot(normal,wo)), eta);
    float3 tint = mat.transmissionParams.x > 1.0e-4 ? 1.0.xxx : saturate3(mat.transmissionColorWeight.xyz);
    return event == kSmsReflection ? F.xxx : (1.0-F)/(eta*eta) * tint;
}
