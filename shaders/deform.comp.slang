// ANI4 + ANI5 renderer half - per-vertex VERTEX DEFORMATION in one dispatch
// per mesh: rest pose -> morph targets -> linear-blend skinning.
//
// CPU references the GPU output must match (Harmonia's test_deform_gpu):
//   * aether::deformMorphs  (ANI5): p' = p + sum(w_t * delta_t) - POSITION-only,
//     normals are NOT re-derived by morphs (documented v1 contract);
//   * aether::deformSkinLbs (ANI4): glTF weight renormalization, positions
//     blended homogeneously (w = 1), normals blended as DIRECTIONS (w = 0) and
//     renormalized with the authored fallback, tangents blended like
//     directions; zero-sum blend weights keep the (morphed) authored vertex.
//
// Composition order matches glTF: morph deltas apply in OBJECT space first,
// then the skin matrices blend the morphed position. The kernel reads the
// immutable REST copy (t_restVertices) and writes the ACTIVE scene-global
// buffer (t_sceneVertices), so re-running at a new tick REPLACES the pose
// instead of composing onto the previous frame's output. For a skinned+morped
// mesh the skin branch blends the morphed position in the same dispatch.
//
// t_skinMatrices rows are the LOCAL-space skinMatrices of aether::SkinPose,
// `inverse(nodeWorld) * jointWorld * inverseBind`, row-major, mul(M, v) -
// deformed vertices stay in the skinned mesh's OBJECT space and the
// instance/TLAS pipeline is untouched.

#include "gpu_types.slang"

[[vk::binding(0, 0)]] StructuredBuffer<uint4> t_boneIndices;
[[vk::binding(1, 0)]] StructuredBuffer<float4> t_boneWeights;
[[vk::binding(2, 0)]] StructuredBuffer<float4x4> t_skinMatrices;
// Immutable rest pose (never written by this pass) -> idempotent across frames.
[[vk::binding(3, 0)]] StructuredBuffer<GpuVertex> t_restVertices;
[[vk::binding(4, 0)]] RWStructuredBuffer<GpuVertex> t_sceneVertices;
// ANI5: per-target position deltas (pad-w rows - the upload pads Vec3 to
// float4 for the 16-byte structured stride) + the staged per-mesh weight
// region (one float per target, re-uploaded every tick).
[[vk::binding(5, 0)]] StructuredBuffer<float4> t_morphDeltas;
[[vk::binding(6, 0)]] StructuredBuffer<float> t_morphWeights;

struct DeformPush {
    uint vertexCount;        // vertices of THIS mesh (sub-mesh range)
    uint sceneVertexOffset;  // this mesh's range start in the scene-global buffer
    uint weightRow;          // absolute row into the skin index/weight arrays
    uint matrixOffset;       // this skin's first matrix, in float4x4 elements
    uint jointCount;         // matrices of this skin (0 = morph-only job)
    uint morphDeltaOffset;   // element offset of this job's target-0 row 0
    uint morphDeltaStride;   // elements per target (the declared mesh count)
    uint morphTargetCount;   // targets of this job (0 = skin-only job)
    uint morphWeightOffset;  // float offset into the staged weight buffer
};
[[vk::push_constant]] DeformPush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.vertexCount)
    {
        return;
    }
    const uint v = tid.x;
    const uint vertexId = pc.sceneVertexOffset + v;
    const GpuVertex src = t_restVertices[vertexId]; // rest pose, not last frame's output

    // 1) ANI5 morphs: POSITION-only deltas over the rest pose (normals and
    // tangents are not re-derived - the documented v1 contract).
    float3 pos = src.position;
    for (uint t = 0; t < pc.morphTargetCount; ++t)
    {
        const float w = t_morphWeights[pc.morphWeightOffset + t];
        if (w != 0.0f)
        {
            pos += w * t_morphDeltas[pc.morphDeltaOffset + t * pc.morphDeltaStride + v].xyz;
        }
    }

    if (pc.jointCount == 0)
    {
        // Morph-only job: write the morphed pose, keep normals/tangents/uv.
        GpuVertex outv = src;
        outv.position = pos;
        t_sceneVertices[vertexId] = outv;
        return;
    }

    // 2) ANI4 skinning over the MORPHED position (glTF order: morph then skin).
    const uint4 bones = t_boneIndices[pc.weightRow + v];
    const float4 raw = t_boneWeights[pc.weightRow + v];
    const float wsum = raw.x + raw.y + raw.z + raw.w;
    if (wsum <= 0.0f)
    {
        // glTF invalid-weights fallback: the skin leaves the (morphed) authored
        // vertex exactly as it is.
        GpuVertex outv = src;
        outv.position = pos;
        t_sceneVertices[vertexId] = outv;
        return;
    }
    const float4 w = raw / wsum;

    float3 blendedP = (0.0f).xxx;
    float3 blendedN = (0.0f).xxx;
    float3 blendedT = (0.0f).xxx;
    for (uint k = 0; k < 4; ++k)
    {
        if (w[k] == 0.0f || bones[k] >= pc.jointCount)
        {
            continue; // the range half is loader-validated; skip defensively
        }
        const float4x4 m = t_skinMatrices[pc.matrixOffset + bones[k]];
        blendedP += w[k] * mul(m, float4(pos, 1.0f)).xyz;
        blendedN += w[k] * mul(m, float4(src.normal, 0.0f)).xyz;
        blendedT += w[k] * mul(m, float4(src.tangentX, src.tangentY, src.tangentZ, 0.0f)).xyz;
    }

    GpuVertex outv = src;
    outv.position = blendedP;
    const float nLen = length(blendedN);
    if (nLen > 1.0e-8f)
    {
        outv.normal = blendedN / nLen;
    }
    const float tLen = length(blendedT);
    if (tLen > 1.0e-8f)
    {
        outv.tangentX = blendedT.x / tLen;
        outv.tangentY = blendedT.y / tLen;
        outv.tangentZ = blendedT.z / tLen;
    }
    t_sceneVertices[vertexId] = outv;
}
