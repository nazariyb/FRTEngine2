#pragma once

#include "CoreTypes.h"
#include "GameInstance.h"
#include "Containers/Array.h"


namespace frt::graphics::mesh
{
SMesh GenerateCube(const Vector3f& Extent, uint32 SubdivisionsCount);
SMesh GenerateSphere(float Radius, uint32 SliceCount, uint32 StackCount);
SMesh GenerateGeosphere(float Radius, uint32 SubdivisionsCount);
SMesh GenerateCylinder(float BottomRadius, float TopRadius, float Height, uint32 SliceCount, uint32 StackCount);
SMesh GenerateGrid(float Width, float Depth, uint32 CellCountWidth, uint32 CellCountDepth);
SMesh GenerateQuad(float Width, float Depth);

void Subdivide(TArray<SVertex>& InOutVertices, TArray<uint32>& InOutIndices, uint32 SubdivisionsCount);
SVertex MidPoint(const SVertex& A, const SVertex& B);


namespace _private
{
	void BuildCylinderCap(
		bool bTop,
		float Radius,
		float Height,
		uint32 SliceCount,
		TArray<SVertex>& OutVertices,
		TArray<uint32>& OutIndices);

#if !defined(FRT_HEADLESS)
	void CreateGpuResources(
		const TArray<SVertex>& Vertices,
		const TArray<uint32>& Indices,
		ComPtr<ID3D12Resource>& OutVertexBufferGpu,
		ComPtr<ID3D12Resource>& OutIndexBufferGpu);
#endif
}
}
