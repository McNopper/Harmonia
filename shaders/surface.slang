// surface.slang — shared OpenPBR surface-frame (texture-map) population.
//
// Both renderers reconstruct a hit (Hyperion via RT builtins in closesthit.slang, Theia via
// RayQuery in gi.comp.slang), then apply the SAME OpenPBR texture-map slot convention and the
// C7 raw-geoNormal backface logic to build the shading frame. That logic lives here, once, so
// it cannot drift — it already did: Theia was missing the base `geometry_tangent` [5]
// anisotropy-frame reorientation that Hyperion applies.
//
// Texture slot convention (matches the host MaterialLibrary / GpuMaterial::textureIndices[2]):
//   [0] base_color   [1] normal            [2] ORM (G=roughness, B=metalness)   [3] emission
//   [4] coat_normal  [5] geometry_tangent  [6] coat_tangent                     [7] map_opacity
//
// Texture sampling is abstracted behind ISurfaceTextureSampler because the renderers bind
// bindless textures differently (Hyperion: combined-image-sampler arrays in RT stages; Theia:
// Sampler2D arrays in compute). Position / shadowPos / mat / wo stay renderer-supplied (the
// caller maps the returned SurfaceFrame onto its SurfaceHit).

import common;       // GpuMaterial, kNoTexture
import math;         // buildTBN

/// Bindless-texture fetch abstraction (mip 0; secondary/GI hits carry no pixel footprint).
interface ISurfaceTextureSampler {
    float4 sampleTex(uint idx, float2 uv);
}

/// Triangle-geometry reconstruction shared by both renderers' hit shaders. Given the
/// world-space triangle (positions/normals/UVs) and barycentric weights, computes the
/// geometric normal, the (degeneracy-guarded, geo-normal-flipped) shading normal, the
/// UV-aligned Lengyel tangent and the interpolated UV. The vertex DATA SOURCES differ
/// (Hyperion: RT builtins + ObjectToWorld3x4; Theia: RayQuery Committed* + instanceTransforms),
/// so each renderer fetches world-space v0/v1/v2 then calls this — the geometry math itself
/// (which already produced subtly different robustness — Theia guarded degenerate normals,
/// Hyperion did not) is now shared and lives once. `bary` packs the weights as (p0,p1,p2).
struct TriangleGeometry {
    float3 geoNormal;
    float3 shadingNormal;
    float3 tangent;
    float2 uv;
};

TriangleGeometry reconstructTriangleGeometry(float3 p0, float3 p1, float3 p2,
                                             float3 n0, float3 n1, float3 n2,
                                             float2 uv0, float2 uv1, float2 uv2,
                                             float3 bary)
{
    TriangleGeometry g;
    g.geoNormal = normalize(cross(p1 - p0, p2 - p0));

    float3 Ns = n0 * bary.x + n1 * bary.y + n2 * bary.z;
    if (dot(Ns, Ns) > 1.0e-8) {
        Ns = normalize(Ns);
    } else {
        Ns = g.geoNormal; // degenerate (zero) vertex normals -> fall back to the geometric normal
    }
    if (dot(Ns, g.geoNormal) < 0.0) {
        Ns = -Ns;
    }
    g.shadingNormal = Ns;

    g.uv = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;

    // UV-aligned tangent from triangle position/UV deltas (Lengyel's method); computed per-hit
    // so normal mapping does not depend on baked vertex tangents.
    float2 duv1 = uv1 - uv0;
    float2 duv2 = uv2 - uv0;
    float3 dp1  = p1 - p0;
    float3 dp2  = p2 - p0;
    float  det  = duv1.x * duv2.y - duv2.x * duv1.y;
    g.tangent = abs(det) > 1.0e-8 ? (dp1 * duv2.y - dp2 * duv1.y) / det : float3(1.0, 0.0, 0.0);
    return g;
}

/// Resolved OpenPBR shading frame after the texture maps are applied.
struct SurfaceFrame {
    float3 normal;     ///< shading normal flipped to face wo (+ normal-mapped)
    float3 geoNormal;  ///< geometric normal flipped to face wo
    float3 T, B;       ///< base tangent frame (anisotropy-reoriented via [5] when bound)
    float3 coatN;
    float3 coatT, coatB;
    bool   backface;   ///< C7: ray hit the back side (inside the dielectric) -> BSDF exit branch
};

/// OpenPBR presence weight α at a hit point: `geometry_opacity × map_opacity.a`
/// (texture slot [7], a Data-space alpha mask).
///
/// This is the ONE definition of "is the surface here?" — OpenPBR's
/// `mix(S_ambient-medium, M_surface, α)`. Every consumer resolves the same
/// number: the estimator's stochastic pass-through gate for camera/indirect
/// rays, the shadow-ray transmittance `1 - α` (spec §Opacity/Transparency), and
/// the rasterizer's alpha cutout. An opacity micromap only lets traversal skip
/// re-deriving it where the answer is provably uniform over a microtriangle —
/// it never replaces this function.
float evalGeometryOpacity(GpuMaterial mat, ISurfaceTextureSampler tex, float2 uv)
{
    float alpha = saturate(mat.opacityFlagsPad.x);
    if (mat.textureIndices2.w != kNoTexture) {
        alpha *= saturate(tex.sampleTex(mat.textureIndices2.w, uv).a);
    }
    return alpha;
}

/// Apply the OpenPBR texture maps to @p mat and the raw hit normals/tangent, returning the
/// resolved shading frame. `wo` is the outgoing direction (toward the camera/previous vertex);
/// `rawShadingNormal` / `rawGeoNormal` are the pre-flip hit normals; `tangent` is the UV-aligned
/// surface tangent. The material is mutated in place (base/ORM/emission/roughness/metalness).
SurfaceFrame applyOpenPbrTextureMaps(inout GpuMaterial mat,
                                     ISurfaceTextureSampler tex,
                                     float3 wo,
                                     float3 rawShadingNormal,
                                     float3 rawGeoNormal,
                                     float3 tangent,
                                     float2 uv)
{
    // [0] base_color, [2] ORM, [3] emission.
    if (mat.textureIndices.x != kNoTexture) {
        mat.baseColorWeight.xyz = tex.sampleTex(mat.textureIndices.x, uv).rgb;
    }
    if (mat.textureIndices.z != kNoTexture) {
        float3 orm = tex.sampleTex(mat.textureIndices.z, uv).rgb;
        mat.specularRoughAnisoIor.x = orm.g;   // specular_roughness
        mat.baseMetalnessDiffRough.x = orm.b;  // base_metalness
    }
    if (mat.textureIndices.w != kNoTexture) {
        mat.emissionColorLum.xyz *= tex.sampleTex(mat.textureIndices.w, uv).rgb;
    }
    // [7] map_opacity — fold the textured mask into the material's presence
    // weight so every downstream reader of opacityFlagsPad.x (the estimator's
    // pass-through gate) sees the resolved α.
    mat.opacityFlagsPad.x = evalGeometryOpacity(mat, tex, uv);

    float3 N = dot(wo, rawShadingNormal) >= 0.0 ? rawShadingNormal : -rawShadingNormal;
    float3 geoN = dot(wo, rawGeoNormal) >= 0.0 ? rawGeoNormal : -rawGeoNormal;

    // [1] normal map — perturb the shading normal in a UV-aligned tangent frame.
    if (mat.textureIndices.y != kNoTexture) {
        float3 nTex = tex.sampleTex(mat.textureIndices.y, uv).xyz * 2.0 - 1.0;
        float3 Tt = tangent - N * dot(N, tangent);
        if (dot(Tt, Tt) > 1.0e-12) {
            Tt = normalize(Tt);
            float3 Bt = cross(N, Tt);
            float3 Np = normalize(Tt * nTex.x + Bt * nTex.y + N * nTex.z);
            N = dot(wo, Np) >= 0.0 ? Np : N; // keep the perturbed normal on the visible side
        }
    }

    float3x3 tbn = buildTBN(N);
    float3 T = tbn[0];
    float3 B = tbn[1];

    // [5] geometry_tangent — reorient the anisotropy frame. The map stores a tangent
    // direction (xy) in the UV-aligned tangent plane; rotate T/B accordingly.
    if (mat.textureIndices2.y != kNoTexture) {
        float2 tDir = tex.sampleTex(mat.textureIndices2.y, uv).xy * 2.0 - 1.0;
        float3 Tuv = tangent - N * dot(N, tangent);
        if (dot(Tuv, Tuv) > 1.0e-12 && dot(tDir, tDir) > 1.0e-12) {
            Tuv = normalize(Tuv);
            float3 Buv = cross(N, Tuv);
            float3 Tn = normalize(Tuv * tDir.x + Buv * tDir.y);
            T = Tn - N * dot(N, Tn);
            if (dot(T, T) > 1.0e-12) {
                T = normalize(T);
                B = cross(N, T);
            } else {
                T = tbn[0];
                B = tbn[1];
            }
        }
    }

    // ── Coat frame ([4] coat_normal, [6] coat_tangent). Defaults to the base frame
    //    (incl. any [5] rotation) so unbound slots reproduce the shared-frame result. ──
    float3 coatN = N;
    float3 coatT = T;
    float3 coatB = B;
    if (mat.textureIndices2.x != kNoTexture) {
        float3 nTex = tex.sampleTex(mat.textureIndices2.x, uv).xyz * 2.0 - 1.0;
        float3 Tt = tangent - N * dot(N, tangent);
        if (dot(Tt, Tt) > 1.0e-12) {
            Tt = normalize(Tt);
            float3 Bt = cross(N, Tt);
            float3 Np = normalize(Tt * nTex.x + Bt * nTex.y + N * nTex.z);
            coatN = dot(wo, Np) >= 0.0 ? Np : N;
            float3x3 coatTbn = buildTBN(coatN);
            coatT = coatTbn[0];
            coatB = coatTbn[1];
        }
    }
    if (mat.textureIndices2.z != kNoTexture) {
        float2 tDir = tex.sampleTex(mat.textureIndices2.z, uv).xy * 2.0 - 1.0;
        float3 Tuv = tangent - coatN * dot(coatN, tangent);
        if (dot(Tuv, Tuv) > 1.0e-12 && dot(tDir, tDir) > 1.0e-12) {
            Tuv = normalize(Tuv);
            float3 Buv = cross(coatN, Tuv);
            float3 Tn = normalize(Tuv * tDir.x + Buv * tDir.y);
            float3 cT = Tn - coatN * dot(coatN, Tn);
            if (dot(cT, cT) > 1.0e-12) {
                coatT = normalize(cT);
                coatB = cross(coatN, coatT);
            }
        }
    }

    SurfaceFrame f;
    f.normal    = N;
    f.geoNormal = geoN;
    f.T = T; f.B = B;
    f.coatN = coatN; f.coatT = coatT; f.coatB = coatB;
    // C7: side flag from the RAW (outward-winding, pre-flip) geometric normal — true when the
    // ray hit the back side (it is inside the dielectric), so the BSDF takes the exit branch.
    f.backface  = dot(wo, rawGeoNormal) < 0.0;
    return f;
}
