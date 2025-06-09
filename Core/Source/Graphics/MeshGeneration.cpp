#include "MeshGeneration.h"

#include "Renderer.h"
#include "Containers/Array.h"


namespace frt::graphics::mesh
{
SMesh GenerateCube (const Vector3f& Extent, uint32 SubdivisionsCount)
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

	const float dHalf = Extent.x * 0.5f;
	const float wHalf = Extent.y * 0.5f;
	const float hHalf = Extent.z * 0.5f;

	// Front face
	v[0] = SVertex
	{
		.Position = { -dHalf, -wHalf, -hHalf }, .Uv = { 0.f, 1.f },
		.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 1.f, 0.f },
	};
	v[1] = SVertex
	{
		.Position = { -dHalf, +wHalf, -hHalf }, .Uv = { 0.f, 0.f },
		.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 1.f, 0.f },
	};
	v[2] = SVertex
	{
		.Position = { +dHalf, +wHalf, -hHalf }, .Uv = { 1.f, 0.f },
		.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 1.f, 0.f },
	};
	v[3] = SVertex
	{
		.Position = { +dHalf, -wHalf, -hHalf }, .Uv = { 1.f, 1.f },
		.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 1.f, 0.f },
	};

	// Back face
	v[4] = SVertex
	{
		.Position = { -dHalf, -wHalf, +hHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, -1.f, 0.f },
	};
	v[5] = SVertex
	{
		.Position = { +dHalf, -wHalf, +hHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, -1.f, 0.f },
	};
	v[6] = SVertex
	{
		.Position = { +dHalf, +wHalf, +hHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, -1.f, 0.f },
	};
	v[7] = SVertex
	{
		.Position = { -dHalf, +wHalf, +hHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, -1.f, 0.f },
	};

	// Top face
	v[8] = SVertex
	{
		.Position = { -dHalf, +wHalf, -hHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
	};
	v[9] = SVertex
	{
		.Position = { -dHalf, +wHalf, +hHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
	};
	v[10] = SVertex
	{
		.Position = { +dHalf, +wHalf, +hHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
	};
	v[11] = SVertex
	{
		.Position = { +dHalf, +wHalf, -hHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
	};

	// Bottom face
	v[12] = SVertex
	{
		.Position = { -dHalf, -wHalf, -hHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 0.f, 0.f, -1.f }, .Tangent = { 0.f, -1.f, 0.f },
	};
	v[13] = SVertex
	{
		.Position = { +dHalf, -wHalf, -hHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, 0.f, -1.f }, .Tangent = { 0.f, -1.f, 0.f },
	};
	v[14] = SVertex
	{
		.Position = { +dHalf, -wHalf, +hHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, 0.f, -1.f }, .Tangent = { 0.f, -1.f, 0.f },
	};
	v[15] = SVertex
	{
		.Position = { -dHalf, -wHalf, +hHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 0.f, 0.f, -1.f }, .Tangent = { 0.f, -1.f, 0.f },
	};

	// Left face
	v[16] = SVertex
	{
		.Position = { -dHalf, -wHalf, +hHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[17] = SVertex
	{
		.Position = { -dHalf, +wHalf, +hHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[18] = SVertex
	{
		.Position = { -dHalf, +wHalf, -hHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};
	v[19] = SVertex
	{
		.Position = { -dHalf, -wHalf, -hHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
	};

	// Right face
	v[20] = SVertex
	{
		.Position = { +dHalf, -wHalf, -hHalf }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { 1.f, 0.f, 0.f },
	};
	v[21] = SVertex
	{
		.Position = { +dHalf, +wHalf, -hHalf }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { 1.f, 0.f, 0.f },
	};
	v[22] = SVertex
	{
		.Position = { +dHalf, +wHalf, +hHalf }, .Uv = { 1.f, 0.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { 1.f, 0.f, 0.f },
	};
	v[23] = SVertex
	{
		.Position = { +dHalf, -wHalf, +hHalf }, .Uv = { 1.f, 1.f },
		.Normal = { 0.f, 1.f, 0.f }, .Tangent = { 1.f, 0.f, 0.f },
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

	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

	return result;
}

SMesh GenerateSphere (float Radius, uint32 SliceCount, uint32 StackCount)
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
		.Position = { 0.f, 0.f, +Radius }, .Uv = { 0.f, 0.f },
		.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
		.Color = { 0.f, 0.f, 0.f, 1.f }
	};

	v[vertexCount - 1] = SVertex // Bottom vertex
	{
		.Position = { 0.f, 0.f, -Radius }, .Uv = { 0.f, 1.f },
		.Normal = { 0.f, 0.f, -1.f }, .Tangent = { 0.f, 1.f, 0.f },
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

			v[vertexIdx++] = SVertex
			{
				.Position =
				{
					Radius * sinf(phi) * sinf(theta),
					Radius * sinf(phi) * cosf(theta),
					Radius * cosf(phi)
				},
				.Uv = { theta / math::TWO_PI, phi / math::PI },
				.Tangent =
				{
					+Radius * sinf(phi) * cosf(theta),
					-Radius * sinf(phi) * sinf(theta),
					0.f
				}
			};
			v[vertexIdx].Tangent.NormalizeUnsafe();
			v[vertexIdx].Normal = v[vertexIdx].Position.GetNormalizedUnsafe();
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

	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

	return result;
}

SMesh GenerateGeosphere (float Radius, uint32 SubdivisionsCount)
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

	v[0].Position = { -X, Z, 0.f };
	v[1].Position = { X, Z, 0.f };
	v[2].Position = { -X, -Z, 0.f };
	v[3].Position = { X, -Z, 0.f };
	v[4].Position = { 0.f, X, Z };
	v[5].Position = { 0.f, -X, Z };
	v[6].Position = { 0.f, X, -Z };
	v[7].Position = { 0.f, -X, -Z };
	v[8].Position = { Z, 0.f, X };
	v[9].Position = { -Z, 0.f, X };
	v[10].Position = { Z, 0.f, -X };
	v[11].Position = { -Z, 0.f, -X };

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
		float theta = atan2f(p.z, p.x);

		// Put in [0, 2pi]
		if (theta < 0.f)
		{
			theta += math::TWO_PI;
		}

		const float phi = acosf(p.y / Radius);

		v[idx].Uv.x = theta / math::TWO_PI;
		v[idx].Uv.y = phi / math::PI;

		// Partial derivative of P with respect to theta
		v[idx].Tangent.x = -Radius * sinf(phi) * sinf(theta);
		v[idx].Tangent.y = Radius * sinf(phi) * cosf(theta);
		v[idx].Tangent.z = 0.f;
		v[idx].Tangent.NormalizeUnsafe();
	}

	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

	return result;
}

SMesh GenerateCylinder ()
{
	SMesh result;
	result.Vertices = TArray<SVertex>(24);
	result.Indices = TArray<uint32>(36);

	auto& v = result.Vertices;
	auto& i = result.Indices;

	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

	return result;
}

SMesh GenerateGrid ()
{
	SMesh result;
	result.Vertices = TArray<SVertex>(24);
	result.Indices = TArray<uint32>(36);

	auto& v = result.Vertices;
	auto& i = result.Indices;

	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

	return result;
}

SMesh GenerateQuad ()
{
	SMesh result;
	result.Vertices = TArray<SVertex>(24);
	result.Indices = TArray<uint32>(36);

	auto& v = result.Vertices;
	auto& i = result.Indices;

	// TODO: temp
	_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

	return result;
}

void Subdivide (TArray<SVertex>& InOutVertices, TArray<uint32>& InOutIndices, uint32 SubdivisionsCount)
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

SVertex MidPoint (const SVertex& A, const SVertex& B)
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

void _private::CreateGpuResources (
	const TArray<SVertex>& Vertices,
	const TArray<uint32>& Indices,
	ComPtr<ID3D12Resource>& OutVertexBufferGpu,
	ComPtr<ID3D12Resource>& OutIndexBufferGpu)
{
	memory::TRefWeak<CRenderer> renderer = GameInstance::GetInstance().GetGraphics();

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

		OutVertexBufferGpu = renderer->CreateBufferAsset(
			vbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, (void*)Vertices.GetData());
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

		OutIndexBufferGpu = renderer->CreateBufferAsset(
			ibDesc, D3D12_RESOURCE_STATE_INDEX_BUFFER, (void*)Indices.GetData());
	}
}
}
