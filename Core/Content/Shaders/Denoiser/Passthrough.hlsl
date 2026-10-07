// CDenoiser disabled: the outputs are the noisy inputs (finite values untouched, NaN / Inf -> 0)
// and the history-length debug texture reads zero, so the renderer can consume the same
// textures either way.

#include "Denoiser/DenoiserCommon.hlsli"

[numthreads(8, 8, 1)]
void main (uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= g_Pass.Size))
	{
		return;
	}
	const int2 pixel = int2(Id.xy);
	RwTex2D(g_Pass.OutDiffuse)[pixel] = DnPassThrough(Tex2D(g_Pass.InDiffuse)[pixel]);
	RwTex2D(g_Pass.OutSpecular)[pixel] = DnPassThrough(Tex2D(g_Pass.InSpecular)[pixel]);
	RwTex2DFloat(g_Pass.HistoryLengthUav)[pixel] = 0.0f;
}
