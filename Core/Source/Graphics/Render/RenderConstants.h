#pragma once

#include "CoreTypes.h"


namespace frt::render::constants
{
static constexpr uint8 SwapChainBufferCount = 2;
static constexpr uint8 FrameResourcesBufferCount = 2;
static constexpr uint32 InitialPassCount = 1;
static constexpr uint32 InitialObjectCount = 1;
static constexpr uint32 InitialMaterialCount = 1;

// Root signature layout (global, reserved space for hybrid extensions).
static constexpr uint32 RootSignatureVersion = 1;
static constexpr uint32 RootParam_BaseColorTexture = 0;
static constexpr uint32 RootParam_ObjectCbv = 1;
static constexpr uint32 RootParam_MaterialCbv = 2;
static constexpr uint32 RootParam_PassCbv = 3;
static constexpr uint32 RootParam_MaterialTextures = 4;
static constexpr uint32 RootParamCount = 5;

static constexpr uint32 RootSpace_Global = 0;
static constexpr uint32 RootSpace_Material = 1;

static constexpr uint32 RootRegister_BaseColorTexture = 0;
static constexpr uint32 RootRegister_ObjectCbv = 0;
static constexpr uint32 RootRegister_MaterialCbv = 1;
static constexpr uint32 RootRegister_PassCbv = 2;
static constexpr uint32 RootRegister_MaterialTextureStart = 0;
static constexpr uint32 RootMaterialTextureCount = 16;

static constexpr uint32 RaytracingRegister_MaterialTextureStart = 1;
static constexpr uint32 RaytracingRegister_VertexBufferSrv =
	RaytracingRegister_MaterialTextureStart + RootMaterialTextureCount;
static constexpr uint32 RaytracingRegister_IndexBufferSrv = RaytracingRegister_VertexBufferSrv + 1;

static constexpr uint32 MaterialTextureSlot_BaseColor = 0;
static constexpr uint32 MaterialTextureIndexInvalid = 0xFFFFFFFFu;

static constexpr uint32 RaytracingHeapSlot_OutputUav = 0;
static constexpr uint32 RaytracingHeapSlot_TlasSrv = 1;
static constexpr uint32 RaytracingHeapSlot_MaterialTextures = 2;
}
