#pragma once
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
void GetAdapterOutputs (IDXGIAdapter1* InAdapter, std::vector<IDXGIOutput*>& OutOutputs);
std::vector<DXGI_MODE_DESC> GetOutputDisplayModes (IDXGIOutput* InOutput, DXGI_FORMAT InFormat);

SDisplayOptions GetDisplayOptions (IDXGIAdapter1* InAdapter);
}
