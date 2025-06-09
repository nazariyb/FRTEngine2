#pragma once
#include <d3d12.h>
#include <dxgi.h>
#include <vector>


namespace frt::graphics
{
struct SDisplayOptions;
}


struct IDXGIAdapter1;
struct IDXGIOutput;


namespace frt::graphics
{
D3D12_SHADER_BYTECODE Dx12LoadShader (const char* Filename);

void GetAdapterOutputs (IDXGIAdapter1* InAdapter, std::vector<IDXGIOutput*>& OutOutputs);
std::vector<DXGI_MODE_DESC> GetOutputDisplayModes (IDXGIOutput* InOutput, DXGI_FORMAT InFormat);

SDisplayOptions GetDisplayOptions (IDXGIAdapter1* InAdapter);
}
