#include "MeshGeneration.h"

#include "Renderer.h"

namespace frt::graphics::mesh
{
	SMesh GenerateCube(const Vector3f& Extent, uint32 SubdivisionsCount)
	{
		SMesh result;

		constexpr uint32 vertexCount = 24u;
		constexpr uint32 indexCount = 36u;

		result.Vertices = memory::NewArray<Vertex>(vertexCount);
		result.Indices = memory::NewArray<uint32>(indexCount);

		auto& v = result.Vertices;
		auto& i = result.Indices;

		const float dHalf = Extent.x * 0.5f;
		const float wHalf = Extent.y * 0.5f;
		const float hHalf = Extent.z * 0.5f;

		// Front face
		v[0] = Vertex
		{
			.Position = { -dHalf, -wHalf, -hHalf }, .Uv = { 0.f, 1.f },
			.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 1.f, 0.f },
		};
		v[1] = Vertex
		{
			.Position = { -dHalf, +wHalf, -hHalf }, .Uv = { 0.f, 0.f },
			.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 1.f, 0.f },
		};
		v[2] = Vertex
		{
			.Position = { +dHalf, +wHalf, -hHalf }, .Uv = { 1.f, 0.f },
			.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 1.f, 0.f },
		};
		v[3] = Vertex
		{
			.Position = { +dHalf, -wHalf, -hHalf }, .Uv = { 1.f, 1.f },
			.Normal = { -1.f, 0.f, 0.f }, .Tangent = { 0.f, 1.f, 0.f },
		};

		// Back face
		v[4] = Vertex
		{
			.Position = { -dHalf, -wHalf, +hHalf }, .Uv = { 1.f, 1.f },
			.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, -1.f, 0.f },
		};
		v[5] = Vertex
		{
			.Position = { +dHalf, -wHalf, +hHalf }, .Uv = { 0.f, 1.f },
			.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, -1.f, 0.f },
		};
		v[6] = Vertex
		{
			.Position = { +dHalf, +wHalf, +hHalf }, .Uv = { 0.f, 0.f },
			.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, -1.f, 0.f },
		};
		v[7] = Vertex
		{
			.Position = { -dHalf, +wHalf, +hHalf }, .Uv = { 1.f, 0.f },
			.Normal = { 1.f, 0.f, 0.f }, .Tangent = { 0.f, -1.f, 0.f },
		};

		// Top face
		v[8] = Vertex
		{
			.Position = { -dHalf, +wHalf, -hHalf }, .Uv = { 0.f, 1.f },
			.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
		};
		v[9] = Vertex
		{
			.Position = { -dHalf, +wHalf, +hHalf }, .Uv = { 0.f, 0.f },
			.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
		};
		v[10] = Vertex
		{
			.Position = { +dHalf, +wHalf, +hHalf }, .Uv = { 1.f, 0.f },
			.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
		};
		v[11] = Vertex
		{
			.Position = { +dHalf, +wHalf, -hHalf }, .Uv = { 1.f, 1.f },
			.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
		};

		// Bottom face
		v[12] = Vertex
		{
			.Position = { -dHalf, -wHalf, -hHalf }, .Uv = { 1.f, 1.f },
			.Normal = { 0.f, 0.f, -1.f }, .Tangent = { 0.f, -1.f, 0.f },
		};
		v[13] = Vertex
		{
			.Position = { +dHalf, -wHalf, -hHalf }, .Uv = { 0.f, 1.f },
			.Normal = { 0.f, 0.f, -1.f }, .Tangent = { 0.f, -1.f, 0.f },
		};
		v[14] = Vertex
		{
			.Position = { +dHalf, -wHalf, +hHalf }, .Uv = { 0.f, 0.f },
			.Normal = { 0.f, 0.f, -1.f }, .Tangent = { 0.f, -1.f, 0.f },
		};
		v[15] = Vertex
		{
			.Position = { -dHalf, -wHalf, +hHalf }, .Uv = { 1.f, 0.f },
			.Normal = { 0.f, 0.f, -1.f }, .Tangent = { 0.f, -1.f, 0.f },
		};

		// Left face
		v[16] = Vertex
		{
			.Position = { -dHalf, -wHalf, +hHalf }, .Uv = { 0.f, 1.f },
			.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
		};
		v[17] = Vertex
		{
			.Position = { -dHalf, +wHalf, +hHalf }, .Uv = { 0.f, 0.f },
			.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
		};
		v[18] = Vertex
		{
			.Position = { -dHalf, +wHalf, -hHalf }, .Uv = { 1.f, 0.f },
			.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
		};
		v[19] = Vertex
		{
			.Position = { -dHalf, -wHalf, -hHalf }, .Uv = { 1.f, 1.f },
			.Normal = { 0.f, -1.f, 0.f }, .Tangent = { -1.f, 0.f, 0.f },
		};

		// Right face
		v[20] = Vertex
		{
			.Position = { +dHalf, -wHalf, -hHalf }, .Uv = { 0.f, 1.f },
			.Normal = { 0.f, 1.f, 0.f }, .Tangent = { 1.f, 0.f, 0.f },
		};
		v[21] = Vertex
		{
			.Position = { +dHalf, +wHalf, -hHalf }, .Uv = { 0.f, 0.f },
			.Normal = { 0.f, 1.f, 0.f }, .Tangent = { 1.f, 0.f, 0.f },
		};
		v[22] = Vertex
		{
			.Position = { +dHalf, +wHalf, +hHalf }, .Uv = { 1.f, 0.f },
			.Normal = { 0.f, 1.f, 0.f }, .Tangent = { 1.f, 0.f, 0.f },
		};
		v[23] = Vertex
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

		result.Vertices = memory::NewArray<Vertex>(vertexCount);
		result.Indices = memory::NewArray<uint32>(indexCount);

		auto& v = result.Vertices;
		auto& i = result.Indices;

		v[0] = Vertex // Top vertex
		{
			.Position = { 0.f, 0.f, +Radius }, .Uv = { 0.f, 0.f },
			.Normal = { 0.f, 0.f, 1.f }, .Tangent = { 0.f, 1.f, 0.f },
			.Color = { 0.f, 0.f, 0.f, 1.f }
		};

		v[vertexCount - 1] = Vertex // Bottom vertex
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

				v[vertexIdx++] = Vertex
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

	SMesh GenerateGeosphere()
	{
		SMesh result;
		result.Vertices = memory::NewArray<Vertex>(24);
		result.Indices = memory::NewArray<uint32>(36);

		auto& v = result.Vertices;
		auto& i = result.Indices;

		// TODO: temp
		_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

		return result;
	}

	SMesh GenerateCylinder()
	{
		SMesh result;
		result.Vertices = memory::NewArray<Vertex>(24);
		result.Indices = memory::NewArray<uint32>(36);

		auto& v = result.Vertices;
		auto& i = result.Indices;

		// TODO: temp
		_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

		return result;
	}

	SMesh GenerateGrid()
	{
		SMesh result;
		result.Vertices = memory::NewArray<Vertex>(24);
		result.Indices = memory::NewArray<uint32>(36);

		auto& v = result.Vertices;
		auto& i = result.Indices;

		// TODO: temp
		_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

		return result;
	}

	SMesh GenerateQuad()
	{
		SMesh result;
		result.Vertices = memory::NewArray<Vertex>(24);
		result.Indices = memory::NewArray<uint32>(36);

		auto& v = result.Vertices;
		auto& i = result.Indices;

		// TODO: temp
		_private::CreateGpuResources(v, i, result.VertexBufferGpu, result.IndexBufferGpu);

		return result;
	}

	void _private::CreateGpuResources(
			const memory::TMemoryHandleArray<Vertex, memory::DefaultAllocator>& vertices,
			const memory::TMemoryHandleArray<uint32, memory::DefaultAllocator>& indices,
			ComPtr<ID3D12Resource>& outVertexBufferGpu,
			ComPtr<ID3D12Resource>& outIndexBufferGpu)
	{
		Renderer& renderer = GameInstance::GetInstance().GetGraphics();

		{
			D3D12_RESOURCE_DESC vbDesc = {};
			vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			vbDesc.Alignment = 0;
			vbDesc.Width = sizeof(Vertex) * vertices.GetNum();
			vbDesc.Height = 1;
			vbDesc.DepthOrArraySize = 1;
			vbDesc.MipLevels = 1;
			vbDesc.Format = DXGI_FORMAT_UNKNOWN;
			vbDesc.SampleDesc.Count = 1;
			vbDesc.SampleDesc.Quality = 0;
			vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			vbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			outVertexBufferGpu = renderer.CreateBufferAsset(
				vbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, vertices.Get());
		}

		{
			D3D12_RESOURCE_DESC ibDesc = {};
			ibDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ibDesc.Alignment = 0;
			ibDesc.Width = sizeof(uint32) * indices.GetNum();
			ibDesc.Height = 1;
			ibDesc.DepthOrArraySize = 1;
			ibDesc.MipLevels = 1;
			ibDesc.Format = DXGI_FORMAT_UNKNOWN;
			ibDesc.SampleDesc.Count = 1;
			ibDesc.SampleDesc.Quality = 0;
			ibDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ibDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			outIndexBufferGpu = renderer.CreateBufferAsset(
				ibDesc, D3D12_RESOURCE_STATE_INDEX_BUFFER, indices.Get());
		}
	}
}
