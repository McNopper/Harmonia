// ANI4 renderer half - per-vertex linear-blend skinning (LBS), written to the
// scene-global vertex buffer so BOTH shading reads and the mesh-local copy
// (the BLAS build input, fed by the host-side vkCmdCopyBuffer after this pass)
// see the deformed pose.
//
// CPU reference: aether::deformSkinLbs (Aether src/aether/format/Skinning.cpp)
// - this kernel must match it exactly:
//   * the 4 authored weights are renormalized when their sum > 0 (glTF rule);
//   * a zero-sum vertex is skipped entirely and stays as authored (the upload
//     already holds the rest pose, and the kernel never touches it);
//   * positions blend homogeneously (w = 1); normals blend as DIRECTIONS
//     (w = 0 - translation must not leak) and are renormalized, falling back to
//     the authored normal when the blend degenerates (len <= 1e-8);
//   * tangents blend like directions (bitangentSign/uv are pose-invariant).
//
// t_skinMatrices rows are the LOCAL-space skinMatrices of aether::SkinPose,
// `inverse(nodeWorld) * jointWorld * inverseBind`, row-major, mul(M, v) -
// so deformed vertices stay in the skinned mesh's OBJECT space and the
// instance/TLAS pipeline is untouched.

#include "gpu_types.slang"

[[vk::binding(0, 0)]] StructuredBuffer<uint4> t_boneIndices;
[[vk::binding(1, 0)]] StructuredBuffer<float4> t_boneWeights;
[[vk::binding(2, 0)]] StructuredBuffer<float4x4> t_skinMatrices;
// Immutable rest pose (never written by this pass) -> idempotent across frames:
// re-running at a new time REPLACES the pose instead of accumulating onto the
// previous frame's deformed output (an in-place read would compose poses).
[[vk::binding(3, 0)]] StructuredBuffer<GpuVertex> t_restVertices;
[[vk::binding(4, 0)]] RWStructuredBuffer<GpuVertex> t_sceneVertices;

struct SkinPush {
    uint vertexCount;       // vertices of THIS mesh
    uint sceneVertexOffset; // this mesh's range start in the scene-global buffer
    uint weightRow;         // absolute row into the skin index/weight arrays
    uint matrixOffset;      // this skin's first matrix, in float4x4 elements
    uint jointCount;        // matrices of this skin
};
[[vk::push_constant]] SkinPush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.vertexCount)
    {
        return;
    }
    const uint v = tid.x;
    const uint4 bones = t_boneIndices[pc.weightRow + v];
    const float4 raw = t_boneWeights[pc.weightRow + v];
    const float wsum = raw.x + raw.y + raw.z + raw.w;
    if (wsum <= 0.0f)
    {
        return; // glTF invalid-weights fallback: the authored vertex stays
    }
    const float4 w = raw / wsum;

    const uint vertexId = pc.sceneVertexOffset + v;
    const GpuVertex src = t_restVertices[vertexId]; // rest pose, not last frame's output

    float3 p = (0.0f).xxx;
    float3 n = (0.0f).xxx;
    float3 t = (0.0f).xxx;
    for (uint k = 0; k < 4; ++k)
    {
        if (w[k] == 0.0f || bones[k] >= pc.jointCount)
        {
            continue; // the range half is loader-validated; skip defensively
        }
        const float4x4 m = t_skinMatrices[pc.matrixOffset + bones[k]];
        p += w[k] * mul(m, float4(src.position, 1.0f)).xyz;
        n += w[k] * mul(m, float4(src.normal, 0.0f)).xyz;
        t += w[k] * mul(m, float4(src.tangentX, src.tangentY, src.tangentZ, 0.0f)).xyz;
    }

    GpuVertex outv = src;
    outv.position = p;
    const float nLen = length(n);
    if (nLen > 1.0e-8f)
    {
        outv.normal = n / nLen;
    }
    const float tLen = length(t);
    if (tLen > 1.0e-8f)
    {
        outv.tangentX = t.x / tLen;
        outv.tangentY = t.y / tLen;
        outv.tangentZ = t.z / tLen;
    }
    t_sceneVertices[vertexId] = outv;
}
