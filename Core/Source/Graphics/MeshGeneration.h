#pragma once

#include "CoreTypes.h"
#include "GameInstance.h"
#include "Containers/Array.h"

namespace frt::graphics::mesh
{
	SMesh GenerateCube(const Vector3f& Extent, uint32 SubdivisionsCount);
	SMesh GenerateSphere(float Radius, uint32 SliceCount, uint32 StackCount);
	SMesh GenerateGeosphere(float Radius, uint32 SubdivisionsCount);
	SMesh GenerateCylinder();
	SMesh GenerateGrid();
	SMesh GenerateQuad();

	void Subdivide(TArray<Vertex>& InOutVertices, TArray<uint32>& InOutIndices, uint32 SubdivisionsCount);
	Vertex MidPoint(const Vertex& A, const Vertex& B);

	namespace _private
	{
		void CreateGpuResources(
			const TArray<Vertex>& vertices,
			const TArray<uint32>& indices,
			ComPtr<ID3D12Resource>& outVertexBufferGpu,
			ComPtr<ID3D12Resource>& outIndexBufferGpu);
	}
}
