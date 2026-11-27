// Fullscreen triangle vertex shader for the tone-mapping graphics pass.
// No vertex buffer needed — three vertices are generated procedurally from SV_VertexID.
// Produces a triangle that covers the entire viewport in Vulkan NDC, with the
// frame's texture coordinates for every vertex.

struct VsOut {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;  // 0..1 across the frame (the glare is sampled with it)
};

[shader("vertex")]
VsOut main(uint vid : SV_VertexID) {
    float x = float(((vid << 1U) & 2U)) * 2.0f - 1.0f;
    float y = float((vid & 2U)) * 2.0f - 1.0f;
    VsOut out;
    out.position = float4(x, y, 0.0f, 1.0f);
    out.uv = float2(x, y) * 0.5f + 0.5f;
    return out;
}
