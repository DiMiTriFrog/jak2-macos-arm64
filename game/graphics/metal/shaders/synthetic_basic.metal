#include <metal_stdlib>

using namespace metal;

struct SyntheticVertex {
  float2 position [[attribute(0)]];
  float4 color [[attribute(1)]];
};

struct SyntheticVertexOut {
  float4 position [[position]];
  float4 color;
};

vertex SyntheticVertexOut synthetic_vertex(SyntheticVertex input [[stage_in]]) {
  SyntheticVertexOut output;
  output.position = float4(input.position, 0.0, 1.0);
  output.color = input.color;
  return output;
}

fragment float4 synthetic_color(SyntheticVertexOut input [[stage_in]]) {
  return input.color;
}
