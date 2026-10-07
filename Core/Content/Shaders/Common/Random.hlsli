#ifndef FRT_RANDOM_HLSLI
#define FRT_RANDOM_HLSLI

// Sampling sources.
//
// * PCG hash (Jarzynski & Olano 2020) for seeding and as the fallback generator.
// * Shuffled, Owen-scrambled 4D Sobol (Burley 2020, "Practical Hash-based Owen Scrambling") for
//   the low path dimensions, indexed by a global sample index that keeps increasing across
//   accumulated frames, so progressive accumulation converges at the scrambled-net rate.
//   Each 4D group gets its own seed, which decorrelates groups while keeping stratification.

uint PcgHash (uint V)
{
	const uint state = V * 747796405u + 2891336453u;
	const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint HashCombine (uint Seed, uint V)
{
	return Seed ^ (PcgHash(V) + 0x9e3779b9u + (Seed << 6u) + (Seed >> 2u));
}

float UintToUnitFloat (uint V)
{
	// 24 mantissa bits -> [0, 1).
	return float(V >> 8u) * (1.0f / 16777216.0f);
}

static const uint kSobolDirections1[32] = {
	0x80000000u, 0xc0000000u, 0xa0000000u, 0xf0000000u, 0x88000000u, 0xcc000000u, 0xaa000000u, 0xff000000u,
	0x80800000u, 0xc0c00000u, 0xa0a00000u, 0xf0f00000u, 0x88880000u, 0xcccc0000u, 0xaaaa0000u, 0xffff0000u,
	0x80008000u, 0xc000c000u, 0xa000a000u, 0xf000f000u, 0x88008800u, 0xcc00cc00u, 0xaa00aa00u, 0xff00ff00u,
	0x80808080u, 0xc0c0c0c0u, 0xa0a0a0a0u, 0xf0f0f0f0u, 0x88888888u, 0xccccccccu, 0xaaaaaaaau, 0xffffffffu,
};
static const uint kSobolDirections2[32] = {
	0x80000000u, 0xc0000000u, 0x60000000u, 0x90000000u, 0xe8000000u, 0x5c000000u, 0x8e000000u, 0xc5000000u,
	0x68800000u, 0x9cc00000u, 0xee600000u, 0x55900000u, 0x80680000u, 0xc09c0000u, 0x60ee0000u, 0x90550000u,
	0xe8808000u, 0x5cc0c000u, 0x8e606000u, 0xc5909000u, 0x6868e800u, 0x9c9c5c00u, 0xeeee8e00u, 0x5555c500u,
	0x8000e880u, 0xc0005cc0u, 0x60008e60u, 0x9000c590u, 0xe8006868u, 0x5c009c9cu, 0x8e00eeeeu, 0xc5005555u,
};
static const uint kSobolDirections3[32] = {
	0x80000000u, 0xc0000000u, 0x20000000u, 0x50000000u, 0xf8000000u, 0x74000000u, 0xa2000000u, 0x93000000u,
	0xd8800000u, 0x25400000u, 0x59e00000u, 0xe6d00000u, 0x78080000u, 0xb40c0000u, 0x82020000u, 0xc3050000u,
	0x208f8000u, 0x51474000u, 0xfbea2000u, 0x75d93000u, 0xa0858800u, 0x914e5400u, 0xdbe79e00u, 0x25db6d00u,
	0x58800080u, 0xe54000c0u, 0x79e00020u, 0xb6d00050u, 0x800800f8u, 0xc00c0074u, 0x200200a2u, 0x50050093u,
};

uint LaineKarrasPermutation (uint X, uint Seed)
{
	X += Seed;
	X ^= X * 0x6c50b47cu;
	X ^= X * 0xb82f1e52u;
	X ^= X * 0xc7afe638u;
	X ^= X * 0x8d22f6e6u;
	return X;
}

uint NestedUniformScramble (uint X, uint Seed)
{
	return reversebits(LaineKarrasPermutation(reversebits(X), Seed));
}

uint4 Sobol4D (uint Index)
{
	uint4 x = uint4(reversebits(Index), 0u, 0u, 0u);
	[unroll]
	for (uint bit = 0u; bit < 32u; ++bit)
	{
		const uint mask = 0u - ((Index >> bit) & 1u);
		x.y ^= mask & kSobolDirections1[bit];
		x.z ^= mask & kSobolDirections2[bit];
		x.w ^= mask & kSobolDirections3[bit];
	}
	return x;
}

float4 ShuffledScrambledSobol4D (uint Index, uint Seed)
{
	const uint shuffled = NestedUniformScramble(Index, Seed);
	uint4 x = Sobol4D(shuffled);
	x.x = NestedUniformScramble(x.x, HashCombine(Seed, 0u));
	x.y = NestedUniformScramble(x.y, HashCombine(Seed, 1u));
	x.z = NestedUniformScramble(x.z, HashCombine(Seed, 2u));
	x.w = NestedUniformScramble(x.w, HashCombine(Seed, 3u));
	return float4(UintToUnitFloat(x.x), UintToUnitFloat(x.y), UintToUnitFloat(x.z), UintToUnitFloat(x.w));
}

/**
 * Stateless per-path sampler. Dimensions are consumed in 4D groups; each group is a
 * shuffled scrambled Sobol point (or a hashed uniform point once LowDiscrepancy is off, e.g.
 * after the first diffuse bounce, where QMC stops paying off).
 */
struct SSampler
{
	uint PixelSeed;      // decorrelates pixels
	uint SampleIndex;    // global sample index for this pixel (accumulation-aware)
	uint Group;          // next 4D group
	bool bLowDiscrepancy;
};

SSampler CreateSampler (uint2 Pixel, uint SampleIndex, uint Salt)
{
	SSampler s;
	s.PixelSeed = PcgHash(PcgHash(Pixel.x + 0x5bd1e995u) + Pixel.y * 0x27d4eb2du + Salt);
	s.SampleIndex = SampleIndex;
	s.Group = 0u;
	s.bLowDiscrepancy = true;
	return s;
}

float4 SampleNext4D (inout SSampler S)
{
	const uint groupSeed = HashCombine(S.PixelSeed, S.Group++);
	if (S.bLowDiscrepancy)
	{
		return ShuffledScrambledSobol4D(S.SampleIndex, groupSeed);
	}
	const uint h0 = PcgHash(groupSeed ^ PcgHash(S.SampleIndex));
	const uint h1 = PcgHash(h0);
	const uint h2 = PcgHash(h1);
	const uint h3 = PcgHash(h2);
	return float4(UintToUnitFloat(h0), UintToUnitFloat(h1), UintToUnitFloat(h2), UintToUnitFloat(h3));
}

float2 SampleNext2D (inout SSampler S) { return SampleNext4D(S).xy; }
float SampleNext1D (inout SSampler S) { return SampleNext4D(S).x; }

// Cheap independent stream for decisions that must not consume structured dimensions
// (stochastic alpha, RIS candidate jitter...).
struct SRng
{
	uint State;
};

SRng CreateRng (uint Seed)
{
	SRng r;
	r.State = PcgHash(Seed);
	return r;
}

float NextFloat (inout SRng R)
{
	R.State = R.State * 747796405u + 2891336453u;
	uint word = ((R.State >> ((R.State >> 28u) + 4u)) ^ R.State) * 277803737u;
	word = (word >> 22u) ^ word;
	return UintToUnitFloat(word);
}

#endif
