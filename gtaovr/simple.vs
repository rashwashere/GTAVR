cbuffer Matrices
{
	matrix mat_mvp;
};

struct VertexInputType
{
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
};

struct PixelInputType
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

PixelInputType SimpleVertexShader(VertexInputType input)
{
	PixelInputType output;
	
	input.position.w = 1.0f;
	
	output.position = mul(input.position, mat_mvp);
	output.texcoord = input.texcoord;
	
	return output;
};