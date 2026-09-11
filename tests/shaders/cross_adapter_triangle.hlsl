struct VertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VertexOutput VSMain(uint vertex_id : SV_VertexID) {
    static const float2 positions[3] = {
        float2(0.0, 0.75),
        float2(0.75, -0.75),
        float2(-0.75, -0.75)
    };
    static const float4 colors[3] = {
        float4(1.0, 0.1, 0.1, 1.0),
        float4(0.1, 1.0, 0.1, 1.0),
        float4(0.1, 0.3, 1.0, 1.0)
    };
    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.color = colors[vertex_id];
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target {
    return input.color;
}
