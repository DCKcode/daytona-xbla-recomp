float4 main(uint index : SV_VertexID) : SV_Position {
  return float4(index == 2 ? 3 : -1, index == 1 ? 3 : -1, 0, 1);
}
