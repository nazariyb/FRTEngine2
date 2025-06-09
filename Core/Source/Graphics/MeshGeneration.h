#pragma once

#include "CoreTypes.h"
#include "GameInstance.h"
#include "Containers/Array.h"


namespace frt::graphics::mesh
{
SMesh GenerateCube (const Vector3f& Extent, uint32 SubdivisionsCount);
SMesh GenerateSphere (float Radius, uint32 SliceCount, uint32 StackCount);
SMesh GenerateGeosphere (float Radius, uint32 SubdivisionsCount);
SMesh GenerateCylinder ();
SMesh GenerateGrid ();
SMesh GenerateQuad ();

void Subdivide (TArray<SVertex>& InOutVertices, TArray<uint32>& InOutIndices, uint32 SubdivisionsCount);
SVertex MidPoint (const SVertex& A, const SVertex& B);


namespace _private
{
	void CreateGpuResources (
		const TArray<SVertex>& Vertices,
		const TArray<uint32>& Indices,
		ComPtr<ID3D12Resource>& OutVertexBufferGpu,
		ComPtr<ID3D12Resource>& OutIndexBufferGpu);
}
}
