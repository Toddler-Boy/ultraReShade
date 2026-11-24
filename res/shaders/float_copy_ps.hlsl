// A texture's stored values into a render target of another format: the source pixel minus the destination pixel is the offset
cbuffer Offset : register(b0)
{
	int2 Shift;
	int2 Unused;
}

Texture2DArray<float4> Part : register(t0);

float4 main(float4 pos : SV_POSITION) : SV_TARGET
{
	return Part.Load(int4(int2(pos.xy) + Shift, 0, 0));
}
