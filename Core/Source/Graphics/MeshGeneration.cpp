#include "MeshGeneration.h"

#include "Render/Renderer.h"
#include "Containers/Array.h"


namespace frt::graphics::mesh
{
static void FlipTriangleWinding (TArray<uint32>& Indices)
{
	for (uint32 idx = 0; idx + 2u < Indices.Count(); idx += 3u)
	{
		const uint32 temp = Indices[idx + 1u];
		Indices[idx + 1u] = Indices[idx + 2u];
		Indices[idx + 2u] = temp;
	}
}

SMesh GenerateCube(const Vector3f& Extent, uint32 SubdivisionsCount)
{
	SMesh result;

	constexpr uint32 vertexCount = 24u;
	constexpr uint32 indexCount = 36u;

	result.Vertices = TArray<SVertex>(vertexCount);
	result.Vertices.SetSizeUninitialized<false>(vertexCount);
	result.Indices = TArray<uint32>(indexCount);
	result.Indices.SetSizeUninitialized<false>(indexCount);

	auto& v = result.Vertices;
	auto& i = result.Indices;

	const float xHalf = Extent.x * 0.5f;
	const float yHalf = Extent.y * 0.5f;
	const float zHalf = Extent.z * 0.5f;

	// Front face
	v[0] = SVertex
	{
		.Position = { +xHalf, -yHalf, +zHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, 0.f, 1.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[1] = SVertex
	{
		.Position = { +xHalf, +yHalf, +zHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, 0.f, 1.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[2] = SVertex
	{
		.Position = { -xHalf, +yHalf, +zHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 0.f, 0.f, 1.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[3] = SVertex
	{
		.Position = { -xHalf, -yHalf, +zHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 0.f, 0.f, 1.f }, .Tangent = { -1.f, 0.f, 0.f },
	};

	// Back face
	v[4] = SVertex
	{
		.Position = { +xHalf, -yHalf, -zHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, 0.f, -1.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[5] = SVertex
	{
		.Position = { -xHalf, -yHalf, -zHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 0.f, 0.f, -1.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[6] = SVertex
	{
		.Position = { -xHalf, +yHalf, -zHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 0.f, 0.f, -1.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[7] = SVertex
	{
		.Position = { +xHalf, +yHalf, -zHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, 0.f, -1.f }, .Tangent = { -1.f, 0.f, 0.f },
	};

	// Top face
	v[8] = SVertex
	{
		.Position = { +xHalf, +yHalf, -zHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[9] = SVertex
	{
		.Position = { -xHalf, +yHalf, -zHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[10] = SVertex
	{
		.Position = { -xHalf, +yHalf, +zHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[11] = SVertex
	{
		.Position = { +xHalf, +yHalf, +zHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};

	// Bottom face
	v[12] = SVertex
	{
		.Position = { +xHalf, -yHalf, -zHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[13] = SVertex
	{
		.Position = { +xHalf, -yHalf, +zHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[14] = SVertex
	{
		.Position = { -xHalf, -yHalf, +zHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[15] = SVertex
	{
		.Position = { -xHalf, -yHalf, -zHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};

	// Left face
	v[16] = SVertex
	{
		.Position = { +xHalf, -yHalf, -zHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, 0.f, 1.f },
	};
	v[17] = SVertex
	{
		.Position = { +xHalf, +yHalf, -zHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, 0.f, 1.f },
	};
	v[18] = SVertex
	{
		.Position = { +xHalf, +yHalf, +zHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, 0.f, 1.f },
	};
	v[19] = SVertex
	{
		.Position = { +xHalf, -yHalf, +zHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, 0.f, 1.f },
	};

	// Right face
	v[20] = SVertex
	{
		.Position = { -xHalf, -yHalf, -zHalf }, .Uv = { 0.f, 1.f },
		.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 0.f, 1.f },
	};
	v[21] = SVertex
	{
		.Position = { -xHalf, -yHalf, +zHalf }, .Uv = { 1.f, 1.f },
		.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 0.f, 1.f },
	};
	v[22] = SVertex
	{
		.Position = { -xHalf, +yHalf, +zHalf }, .Uv = { 1.f, 0.f },
		.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 0.f, 1.f },
	};
	v[23] = SVertex
	{
		.Position = { -xHalf, +yHalf, -zHalf }, .Uv = { 0.f, 0.f },
		.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 0.f, 1.f },
	};

	// Front face
	i[0] = 0; i[1] = 1; i[2] = 2;
	i[3] = 0; i[4] = 2; i[5] = 3;

	// Back face
	i[6] = 4; i[7]  = 5; i[8]  = 6;
	i[9] = 4; i[10] = 6; i[11] = 7;

	// Top face
	i[12] = 8; i[13] =  9; i[14] = 10;
	i[15] = 8; i[16] = 10; i[17] = 11;

	// Bottom face
	i[18] = 12; i[19] = 13; i[20] = 14;
	i[21] = 12; i[22] = 14; i[23] = 15;

	// Left face
	i[24] = 16; i[25] = 17; i[26] = 18;
	i[27] = 16; i[28] = 18; i[29] = 19;

	// Right face
	i[30] = 20; i[31] = 21; i[32] = 22;
	i[33] = 20; i[34] = 22; i[35] = 23;

#if !defined(FRT_HEADLESS)
	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);
#endif
	return result;
}

SMesh GenerateSphere(float Radius, uint32 SliceCount, uint32 StackCount)
{
	frt_assert(SliceCount > 1u);
	frt_assert(StackCount > 1u);

	SMesh result;

	constexpr uint32 polesCount = 2u;
	const uint32 vertexCount = (SliceCount + 1u) * (StackCount - 1u) + polesCount;

	const uint32 poleStacksIndexCount = SliceCount * 3u * polesCount;
	const uint32 innerStacksIndexCount = (StackCount - polesCount) * SliceCount * 6u;
	const uint32 indexCount = poleStacksIndexCount + innerStacksIndexCount;

	result.Vertices.SetSize<true>(vertexCount);
	result.Indices.SetSize<true>(indexCount);

	auto& v = result.Vertices;
	auto& i = result.Indices;

	v[0] = SVertex // Top vertex
	{
		.Position = { 0.f, +Radius, 0.f }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { 1.f, 0.f, 0.f },
		.Color = { 0.f, 0.f, 0.f, 1.f }
	};

	v[vertexCount - 1] = SVertex // Bottom vertex
	{
		.Position = { 0.f, -Radius, 0.f }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, -1.f, 0.f }, .Tangent = { 1.f, 0.f, 0.f },
		.Color = { 0.f, 0.f, 0.f, 1.f }
	};

	const float phiStep = math::PI / static_cast<float>(StackCount);
	const float thetaStep = math::TWO_PI / static_cast<float>(SliceCount);

	uint32 vertexIdx = 1u;
	for (uint32 stackIdx = 1u; stackIdx <= StackCount - 1u; ++stackIdx)
	{
		const float phi = phiStep * static_cast<float>(stackIdx);

		for (uint32 sliceIdx = 0u; sliceIdx <= SliceCount; ++sliceIdx)
		{
			const float theta = thetaStep * static_cast<float>(sliceIdx);

			v[vertexIdx] = SVertex
			{
				.Position =
				{
					Radius * sinf(phi) * sinf(theta),
					Radius * cosf(phi),
					Radius * sinf(phi) * cosf(theta)
				},
				.Uv = { theta / math::TWO_PI, phi / math::PI },
				.Tangent =
				{
					Radius * sinf(phi) * cosf(theta),
					0.f,
					-Radius * sinf(phi) * sinf(theta)
				}
			};
			v[vertexIdx].Tangent.NormalizeUnsafe();
			v[vertexIdx].Normal = v[vertexIdx].Position.GetNormalizedUnsafe();
			vertexIdx++;
		}
	}

	// Top stack
	for (uint32 sliceIdx = 1; sliceIdx <= SliceCount; ++sliceIdx)
	{
		i[(sliceIdx - 1u) * 3u] = 0u;
		i[(sliceIdx - 1u) * 3u + 1u] = sliceIdx + 1u;
		i[(sliceIdx - 1u) * 3u + 2u] = sliceIdx;
	}

	// Inner stacks
	uint32 baseIdx = 1u; // Offset for nort pole
	const uint32 ringVertexCount = SliceCount + 1u;
	uint32 curIdx = SliceCount * 3u;
	for (uint32 stackIdx = 0; stackIdx < StackCount - 2u; ++stackIdx)
	{
		for (uint32 sliceIdx = 0; sliceIdx < SliceCount; ++sliceIdx)
		{
			// curIdx = SliceCount * 3u + stackIdx * SliceCount * 6u + sliceIdx * 6u;

			i[curIdx++] = baseIdx + stackIdx * ringVertexCount + sliceIdx;
			i[curIdx++] = baseIdx + stackIdx * ringVertexCount + sliceIdx + 1u;
			i[curIdx++] = baseIdx + (stackIdx + 1u) * ringVertexCount + sliceIdx;

			i[curIdx++] = baseIdx + (stackIdx + 1u) * ringVertexCount + sliceIdx;
			i[curIdx++] = baseIdx + stackIdx * ringVertexCount + sliceIdx + 1u;
			i[curIdx++] = baseIdx + (stackIdx + 1u) * ringVertexCount + sliceIdx + 1u;
		}
	}

	// Bottom stack
	const uint32 southPoleIdx = vertexCount - 1u;
	baseIdx = southPoleIdx - ringVertexCount;
	for (uint32 sliceIdx = 0; sliceIdx < SliceCount; ++sliceIdx)
	{
		i[curIdx++] = southPoleIdx;
		i[curIdx++] = baseIdx + sliceIdx;
		i[curIdx++] = baseIdx + sliceIdx + 1u;
	}

	FlipTriangleWinding(i);

#if !defined(FRT_HEADLESS)
	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);
#endif
	return result;
}

SMesh GenerateGeosphere(float Radius, uint32 SubdivisionsCount)
{
	SMesh result;

	const uint32 subdivisions = math::Min<uint32>(SubdivisionsCount, 6u);

	constexpr uint32 initialVertexCount = 12u;

	result.Vertices.SetSize<true>(initialVertexCount);

	auto& v = result.Vertices;
	auto& i = result.Indices;

	// Approximate a sphere by tessellating an icosahedron.

	constexpr float X = 0.525731f;
	constexpr float Z = 0.850651f;

	const auto toLuf = [] (float x, float y, float z)
	{
		return Vector3f(-y, z, x);
	};

	v[0].Position = toLuf(-X, Z, 0.f);
	v[1].Position = toLuf(X, Z, 0.f);
	v[2].Position = toLuf(-X, -Z, 0.f);
	v[3].Position = toLuf(X, -Z, 0.f);
	v[4].Position = toLuf(0.f, X, Z);
	v[5].Position = toLuf(0.f, -X, Z);
	v[6].Position = toLuf(0.f, X, -Z);
	v[7].Position = toLuf(0.f, -X, -Z);
	v[8].Position = toLuf(Z, 0.f, X);
	v[9].Position = toLuf(-Z, 0.f, X);
	v[10].Position = toLuf(Z, 0.f, -X);
	v[11].Position = toLuf(-Z, 0.f, -X);

	i =
		{
			1u, 4u, 0u,    4u,9u,0u,   4u,5u, 9u,   8u,5u,4u,    1u, 8u,4u,
			1u, 10u,8u,   10u,3u,8u,   8u,3u, 5u,   3u,2u,5u,    3u, 7u,2u,
			3u, 10u,7u,   10u,6u,7u,   6u,11u,7u,   6u,0u,11u,   6u, 1u,0u,
			10u,1u, 6u,   11u,0u,9u,   2u,11u,9u,   5u,2u,9u,    11u,2u,7u,
		};

	Subdivide(v, i, subdivisions);

	// Project vertices onto sphere and scale
	for (uint32 idx = 0; idx < v.Count(); ++idx)
	{
		const Vector3f n = v[idx].Position.GetNormalizedUnsafe();
		const Vector3f p = Radius * n;

		v[idx].Position = p;
		v[idx].Normal = n;

		// Derive texture coordinates from spherical coordinates
		float theta = atan2f(p.x, p.z);

		// Put in [0, 2pi]
		if (theta < 0.f)
		{
			theta += math::TWO_PI;
		}

		const float phi = acosf(p.y / Radius);

		v[idx].Uv.x = theta / math::TWO_PI;
		v[idx].Uv.y = phi / math::PI;

		// Partial derivative of P with respect to theta
		v[idx].Tangent.x = Radius * sinf(phi) * cosf(theta);
		v[idx].Tangent.y = 0.f;
		v[idx].Tangent.z = -Radius * sinf(phi) * sinf(theta);
		v[idx].Tangent.NormalizeUnsafe();
	}

	FlipTriangleWinding(i);

#if !defined(FRT_HEADLESS)
	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);
#endif
	return result;
}

SMesh GenerateCylinder(float BottomRadius, float TopRadius, float Height, uint32 SliceCount, uint32 StackCount)
{
	SMesh result;

	// result.Vertices = TArray<SVertex>(24);
	// result.Indices = TArray<uint32>(36);

	auto& v = result.Vertices;
	auto& i = result.Indices;

	const float stackHeight = Height / static_cast<float>(StackCount);
	const float radiusStep = (TopRadius - BottomRadius) / static_cast<float>(StackCount);

	const uint32 ringCount = StackCount + 1u;

	for (uint32 stackIdx = 0u; stackIdx < ringCount; ++stackIdx)
	{
		const float y = -Height * 0.5f + static_cast<float>(stackIdx) * stackHeight;
		const float radius = BottomRadius + static_cast<float>(stackIdx) * radiusStep;

		for (uint32 sliceIdx = 0u; sliceIdx <= SliceCount; ++sliceIdx)
		{
			const float theta = math::TWO_PI * static_cast<float>(sliceIdx) / static_cast<float>(SliceCount);
			const float cosTheta = cosf(theta);
			const float sinTheta = sinf(theta);

			SVertex vertex;
			vertex.Position = Vector3f::ForwardVector * radius * sinTheta
							+ Vector3f::UpVector * y
							+ Vector3f::RightVector * radius * cosTheta;
			vertex.Uv.x = static_cast<float>(sliceIdx) / static_cast<float>(SliceCount);
			vertex.Uv.y = static_cast<float>(stackIdx) / static_cast<float>(StackCount);

			// Derivative by azimuth angle (theta), normalized.
			vertex.Tangent = (Vector3f::ForwardVector * cosTheta
							+ Vector3f::RightVector * -sinTheta)
							.GetNormalizedUnsafe();

			// Analytic cone side normal (outward). Reduces seam/tilt artifacts.
			const float dr = BottomRadius - TopRadius;
			vertex.Normal = (Vector3f::RightVector * cosTheta
							+ Vector3f::UpVector * (dr / Height)
							+ Vector3f::ForwardVector * sinTheta)
							.GetNormalizedUnsafe();

			v.Add(vertex);
		}
	}

	const uint32 ringVertexCount = SliceCount + 1u;

	for (uint32 stackIdx = 0u; stackIdx < StackCount; ++stackIdx)
	{
		for (uint32 sliceIdx = 0u; sliceIdx < SliceCount; ++sliceIdx)
		{
			const uint32 baseIndex = stackIdx * ringVertexCount + sliceIdx;

			i.Add(baseIndex);
			i.Add(baseIndex + ringVertexCount);
			i.Add(baseIndex + 1u);

			i.Add(baseIndex + 1u);
			i.Add(baseIndex + ringVertexCount);
			i.Add(baseIndex + ringVertexCount + 1u);
		}
	}

	_private::BuildCylinderCap(true, TopRadius, Height, SliceCount, v, i);
	_private::BuildCylinderCap(false, BottomRadius, Height, SliceCount, v, i);

#if !defined(FRT_HEADLESS)
	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);
#endif
	return result;
}

SMesh GenerateGrid(float Width, float Depth, uint32 CellCountWidth, uint32 CellCountDepth)
{
	SMesh result;

	auto& v = result.Vertices;
	auto& i = result.Indices;

	v.SetSize<true>(CellCountWidth * CellCountDepth);

	const uint32 faceCount = (CellCountWidth - 1u) * (CellCountDepth - 1u) * 2u;
	i.SetSize<true>(faceCount * 3u);

	const float halfWidth = Width * 0.5f;
	const float halfDepth = Depth * 0.5f;

	const float dx = Width / static_cast<float>(CellCountWidth - 1u);
	const float dz = Depth / static_cast<float>(CellCountDepth - 1u);

	const float du = 1.f / static_cast<float>(CellCountWidth - 1u);
	const float dv = 1.f / static_cast<float>(CellCountDepth - 1u);

	for (uint32 z = 0u; z < CellCountDepth; ++z)
	{
		for (uint32 x = 0u; x < CellCountWidth; ++x)
		{
			const uint32 idx = z * CellCountWidth + x;

			v[idx].Position = Vector3f::ForwardVector * (halfDepth - static_cast<float>(z) * dz)
							+ Vector3f::UpVector * 0.f
							+ Vector3f::RightVector * (-halfWidth + static_cast<float>(x) * dx);

			v[idx].Uv.x = static_cast<float>(x) * du;
			v[idx].Uv.y = static_cast<float>(z) * dv;

			v[idx].Normal = Vector3f::UpVector;
			v[idx].Tangent = Vector3f::RightVector;
		}
	}

	uint32 k = 0u;
	for (uint32 z = 0u; z < CellCountDepth - 1u; ++z)
	{
		for (uint32 x = 0u; x < CellCountWidth - 1u; ++x)
		{
			const uint32 baseIndex = z * CellCountWidth + x;

			// First triangle
			i[k++] = baseIndex;
			i[k++] = baseIndex + CellCountWidth;
			i[k++] = baseIndex + 1u;

			// Second triangle
			i[k++] = baseIndex + 1u;
			i[k++] = baseIndex + CellCountWidth;
			i[k++] = baseIndex + CellCountWidth + 1u;
		}
	}

#if !defined(FRT_HEADLESS)
	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);
#endif
	return result;
}

SMesh GenerateQuad(float Width, float Depth)
{
	SMesh result;

	auto& v = result.Vertices;
	auto& i = result.Indices;

	v.SetSize<true>(4u);
	i.SetSize<true>(6u);

	const float halfWidth = Width * 0.5f;
	const float halfDepth = Depth * 0.5f;

	v[0] = SVertex
	{
		.Position = Vector3f::ForwardVector * halfDepth + Vector3f::LeftVector * halfWidth,
		.Uv = { 0.f, 1.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f }
	};

	v[1] = SVertex
	{
		.Position = Vector3f::BackwardVector * halfDepth + Vector3f::LeftVector * halfWidth,
		.Uv = { 0.f, 0.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f }
	};

	v[2] = SVertex
	{
		.Position = Vector3f::BackwardVector * halfDepth + Vector3f::RightVector * halfWidth,
		.Uv = { 1.f, 0.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f }
	};

	v[3] = SVertex
	{
		.Position = Vector3f::ForwardVector * halfDepth + Vector3f::RightVector * halfWidth,
		.Uv = { 1.f, 1.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f }
	};

	i[0] = 0u; i[1] = 1u; i[2] = 2u;
	i[3] = 0u; i[4] = 2u; i[5] = 3u;

#if !defined(FRT_HEADLESS)
	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);
#endif
	return result;
}

void Subdivide(TArray<SVertex>& InOutVertices, TArray<uint32>& InOutIndices, uint32 SubdivisionsCount)
{
	if (InOutVertices.Count() == 0 || InOutIndices.Count() == 0)
	{
		return; // Nothing to subdivide.
	}

	for (uint32 sub = 0; sub < SubdivisionsCount; ++sub)
	{
		// Each subdivision will add 3 new vertices and 6 new indices per triangle.
		// The number of triangles will increase by 4x.
		// So, we need to generate new vertices and indices based on the current ones.
		TArray<SVertex> oldVertices = InOutVertices;
		TArray<uint32> oldIndices = InOutIndices;

		TArray<SVertex>& newVertices = InOutVertices;
		TArray<uint32>& newIndices = InOutIndices;

		newVertices.Clear();
		newIndices.Clear();

		//       v1
		//       *
		//      / \
		//     /   \
		//  m0*-----*m1
		//   / \   / \
		//  /   \ /   \
		// *-----*-----*
		// v0    m2     v2

		const uint32 numTris = oldIndices.size() / 3u;
		for (uint32 i = 0; i < numTris; ++i)
		{
			SVertex v0 = oldVertices[oldIndices[i * 3 + 0]];
			SVertex v1 = oldVertices[oldIndices[i * 3 + 1]];
			SVertex v2 = oldVertices[oldIndices[i * 3 + 2]];

			//
			// Generate the midpoints.
			//

			SVertex m0 = MidPoint(v0, v1);
			SVertex m1 = MidPoint(v1, v2);
			SVertex m2 = MidPoint(v0, v2);

			//
			// Add new geometry.
			//

			newVertices.Add(v0); // 0
			newVertices.Add(v1); // 1
			newVertices.Add(v2); // 2
			newVertices.Add(m0); // 3
			newVertices.Add(m1); // 4
			newVertices.Add(m2); // 5

			newIndices.Add(i * 6 + 0);
			newIndices.Add(i * 6 + 3);
			newIndices.Add(i * 6 + 5);

			newIndices.Add(i * 6 + 3);
			newIndices.Add(i * 6 + 4);
			newIndices.Add(i * 6 + 5);

			newIndices.Add(i * 6 + 5);
			newIndices.Add(i * 6 + 4);
			newIndices.Add(i * 6 + 2);

			newIndices.Add(i * 6 + 3);
			newIndices.Add(i * 6 + 1);
			newIndices.Add(i * 6 + 4);
		}
	}
}

SVertex MidPoint(const SVertex& A, const SVertex& B)
{
	return SVertex
	{
		.Position = (A.Position + B.Position) * 0.5f,
		.Uv = (A.Uv + B.Uv) * 0.5f,
		.Normal = (A.Normal + B.Normal).GetNormalizedUnsafe(),
		.Tangent = (A.Tangent + B.Tangent).GetNormalizedUnsafe(),
		// .Color = (A.Color + B.Color) * 0.5f // TODO: color
	};
}

void _private::BuildCylinderCap(
	bool bTop,
	float Radius,
	float Height,
	uint32 SliceCount,
	TArray<SVertex>& OutVertices,
	TArray<uint32>& OutIndices)
{
	const float normalY = (bTop ? 1.f : -1.f);
	const float y = Height * 0.5f * normalY;

	const uint32 baseIndex = OutVertices.Count();
	for (uint32 sliceIdx = 0; sliceIdx <= SliceCount; ++sliceIdx)
	{
		const float theta = math::TWO_PI * static_cast<float>(sliceIdx) / static_cast<float>(SliceCount);
		const float x = Radius * sinf(theta);
		const float z = Radius * cosf(theta);

		// vertex.Tangent = Vector3f(-sinf(theta), 0.f, cosf(theta));

		OutVertices.Add(
			SVertex
			{
				.Position = Vector3f::ForwardVector * x
							+ Vector3f::UpVector * y
							+ Vector3f::RightVector * z,
				.Uv = Vector2f(
					Radius > 0.0f ? (z / (2.0f * Radius) + 0.5f) : 0.5f,
					Radius > 0.0f ? (x / (2.0f * Radius) + 0.5f) : 0.5f),
				.Normal = Vector3f::UpVector * normalY,
				.Tangent = Vector3f::ForwardVector
			});
	}

	OutVertices.Add(
		SVertex
		{
			.Position = Vector3f::UpVector * y,
			.Uv = Vector2f(0.5f, 0.5f),
			.Normal = Vector3f::UpVector * normalY,
			.Tangent = Vector3f::ForwardVector
		});

	const uint32 centerIndex = OutVertices.Count() - 1u;

	for (uint32 sliceIdx = 0; sliceIdx < SliceCount; ++sliceIdx)
	{
		const uint32 i0 = baseIndex + sliceIdx;
		const uint32 i1 = baseIndex + sliceIdx + 1u;

		if (bTop)
		{
			OutIndices.Add(centerIndex);
			OutIndices.Add(i1);
			OutIndices.Add(i0);
		}
		else
		{
			OutIndices.Add(centerIndex);
			OutIndices.Add(i0);
			OutIndices.Add(i1);
		}
	}
}

#if !defined(FRT_HEADLESS)
void _private::CreateGpuResources(
	const TArray<SVertex>& Vertices,
	const TArray<uint32>& Indices,
	ComPtr<ID3D12Resource>& OutVertexBufferGpu,
	ComPtr<ID3D12Resource>& OutIndexBufferGpu)
{
	memory::TRefWeak<CRenderer> renderer = GameInstance::GetInstance().GetRenderer();

	{
		D3D12_RESOURCE_DESC vbDesc = {};
		vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		vbDesc.Alignment = 0;
		vbDesc.Width = sizeof(SVertex) * Vertices.Count();
		vbDesc.Height = 1;
		vbDesc.DepthOrArraySize = 1;
		vbDesc.MipLevels = 1;
		vbDesc.Format = DXGI_FORMAT_UNKNOWN;
		vbDesc.SampleDesc.Count = 1;
		vbDesc.SampleDesc.Quality = 0;
		vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		vbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		OutVertexBufferGpu = renderer->CreateBufferAsset(vbDesc);
		renderer->EnqueueBufferUpload(
			OutVertexBufferGpu.Get(),
			vbDesc.Width,
			Vertices.GetData(),
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	{
		D3D12_RESOURCE_DESC ibDesc = {};
		ibDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		ibDesc.Alignment = 0;
		ibDesc.Width = sizeof(uint32) * Indices.Count();
		ibDesc.Height = 1;
		ibDesc.DepthOrArraySize = 1;
		ibDesc.MipLevels = 1;
		ibDesc.Format = DXGI_FORMAT_UNKNOWN;
		ibDesc.SampleDesc.Count = 1;
		ibDesc.SampleDesc.Quality = 0;
		ibDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ibDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		OutIndexBufferGpu = renderer->CreateBufferAsset(ibDesc);
		renderer->EnqueueBufferUpload(
			OutIndexBufferGpu.Get(),
			ibDesc.Width,
			Indices.GetData(),
			D3D12_RESOURCE_STATE_INDEX_BUFFER |
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
}
#endif
}
