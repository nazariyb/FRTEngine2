#pragma once

#include "CoreTypes.h"
#include "GameInstance.h"
#include "Containers/Array.h"

namespace frt::graphics::mesh
{
	SMesh GenerateCube(const Vector3f& Extent, uint32 SubdivisionsCount);
	SMesh GenerateSphere(float Radius, uint32 SliceCount, uint32 StackCount);
	SMesh GenerateGeosphere();
	SMesh GenerateCylinder();
	SMesh GenerateGrid();
	SMesh GenerateQuad();

	namespace _private
	{
		void CreateGpuResources(
			const TArray<Vertex>& vertices,
			const TArray<uint32>& indices,
			ComPtr<ID3D12Resource>& outVertexBufferGpu,
			ComPtr<ID3D12Resource>& outIndexBufferGpu);
	}
}
