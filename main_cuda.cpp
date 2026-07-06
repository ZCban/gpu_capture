#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <torch/extension.h>

namespace py = pybind11;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "cudart.lib")

bool g_useRGBA = true;
bool g_useZoom = true;

ID3D11Device*              pDevice               = nullptr;
ID3D11DeviceContext*       pDeviceContext         = nullptr;
IDXGIOutputDuplication*    pDeskDupl              = nullptr;
ID3D11Texture2D*           pGpuTargetTexture      = nullptr;
ID3D11ComputeShader*       pComputeShader         = nullptr;
ID3D11Buffer*              pParamBuffer           = nullptr;
ID3D11Buffer*              pGpuFloatOutputBuffer  = nullptr;
ID3D11ShaderResourceView*  pInputSRV              = nullptr;
ID3D11UnorderedAccessView* pOutputUAV             = nullptr;

cudaGraphicsResource*      g_cudaResource         = nullptr;
torch::Tensor              g_cudaResult;

UINT cropLeft   = 0, cropTop = 0;
UINT cropWidth  = 640;
UINT cropHeight = 640;

const char* computeShaderSrc = R"hlsl(
Texture2D<float4> InputTexture : register(t0);
RWStructuredBuffer<float> OutputBuffer : register(u0);

cbuffer Params : register(b0) {
    uint Width;
    uint Height;
    int UseZoom;
    int UseRGBA;
};

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Width || id.y >= Height) return;

    uint2 sampleCoord = id.xy;

    if (UseZoom == 1) {
        uint crop_size = (uint)(Width / 1.25);
        uint start = (Width - crop_size) / 2;
        sampleCoord = uint2(
            start + (id.x * crop_size) / Width,
            start + (id.y * crop_size) / Height
        );
    }

    float4 pixel = InputTexture.mips[0][sampleCoord];

    uint planeSize = Width * Height;
    uint pixelIdx  = id.y * Width + id.x;

    if (UseRGBA == 1) {
        OutputBuffer[pixelIdx]                 = pixel.b;
        OutputBuffer[planeSize + pixelIdx]     = pixel.g;
        OutputBuffer[2 * planeSize + pixelIdx] = pixel.r;
    } else {
        OutputBuffer[pixelIdx]                 = pixel.r;
        OutputBuffer[planeSize + pixelIdx]     = pixel.g;
        OutputBuffer[2 * planeSize + pixelIdx] = pixel.b;
    }
}
)hlsl";

void cleanup()
{
    if (g_cudaResource) {
        cudaGraphicsUnregisterResource(g_cudaResource);
        g_cudaResource = nullptr;
    }
    g_cudaResult = torch::Tensor(); // reset tensor

    if (pOutputUAV)            { pOutputUAV->Release();            pOutputUAV = nullptr; }
    if (pInputSRV)             { pInputSRV->Release();             pInputSRV = nullptr; }
    if (pGpuFloatOutputBuffer) { pGpuFloatOutputBuffer->Release(); pGpuFloatOutputBuffer = nullptr; }
    if (pComputeShader)        { pComputeShader->Release();        pComputeShader = nullptr; }
    if (pParamBuffer)          { pParamBuffer->Release();          pParamBuffer = nullptr; }
    if (pGpuTargetTexture)     { pGpuTargetTexture->Release();     pGpuTargetTexture = nullptr; }
    if (pDeskDupl)             { pDeskDupl->Release();             pDeskDupl = nullptr; }
    if (pDeviceContext)        { pDeviceContext->Release();        pDeviceContext = nullptr; }
    if (pDevice)               { pDevice->Release();              pDevice = nullptr; }
}

bool initialize_capture(UINT width, UINT height, bool use_rgba, bool use_zoom)
{
    cleanup();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_useRGBA   = use_rgba;
    g_useZoom   = use_zoom;
    cropWidth   = width;
    cropHeight  = height;

    int screenWidth  = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    cropLeft = (screenWidth  - cropWidth)  / 2;
    cropTop  = (screenHeight - cropHeight) / 2;

    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };

    if (FAILED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels, 1, D3D11_SDK_VERSION,
        &pDevice, &featureLevel, &pDeviceContext)))
        return false;

    IDXGIDevice* pDxgiDevice = nullptr;
    pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDxgiDevice);

    IDXGIAdapter* pDxgiAdapter = nullptr;
    pDxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&pDxgiAdapter);

    pDxgiDevice->Release();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = cropWidth;
    desc.Height           = cropHeight;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    pDevice->CreateTexture2D(&desc, nullptr, &pGpuTargetTexture);

    IDXGIOutput* pDxgiOutput = nullptr;
    pDxgiAdapter->EnumOutputs(0, &pDxgiOutput);

    IDXGIOutput1* pDxgiOutput1 = nullptr;
    pDxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pDxgiOutput1);

    pDxgiOutput1->DuplicateOutput(pDevice, &pDeskDupl);

    pDxgiOutput1->Release();
    pDxgiOutput->Release();
    pDxgiAdapter->Release();

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth  = 16;
    cbDesc.Usage      = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags  = D3D11_BIND_CONSTANT_BUFFER;

    pDevice->CreateBuffer(&cbDesc, nullptr, &pParamBuffer);

    struct Params { UINT w, h; int zoom; int rgba; } params;
    params.w    = cropWidth;
    params.h    = cropHeight;
    params.zoom = g_useZoom ? 1 : 0;
    params.rgba = g_useRGBA ? 1 : 0;

    pDeviceContext->UpdateSubresource(pParamBuffer, 0, nullptr, &params, 0, 0);

    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.ByteWidth           = cropWidth * cropHeight * 3 * sizeof(float);
    bufDesc.Usage               = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags           = D3D11_BIND_UNORDERED_ACCESS;
    bufDesc.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.StructureByteStride = sizeof(float);

    pDevice->CreateBuffer(&bufDesc, nullptr, &pGpuFloatOutputBuffer);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension      = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Format             = DXGI_FORMAT_UNKNOWN;
    uavDesc.Buffer.NumElements = cropWidth * cropHeight * 3;

    pDevice->CreateUnorderedAccessView(
        pGpuFloatOutputBuffer, &uavDesc, &pOutputUAV);

    pDevice->CreateShaderResourceView(pGpuTargetTexture, nullptr, &pInputSRV);

    ID3DBlob* shaderBlob = nullptr;
    D3DCompile(computeShaderSrc, strlen(computeShaderSrc),
        nullptr, nullptr, nullptr,
        "CSMain", "cs_5_0", 0, 0,
        &shaderBlob, nullptr);

    pDevice->CreateComputeShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        nullptr, &pComputeShader);

    shaderBlob->Release();

    // imposta una volta sola
    pDeviceContext->CSSetShader(pComputeShader, nullptr, 0);
    pDeviceContext->CSSetShaderResources(0, 1, &pInputSRV);
    pDeviceContext->CSSetUnorderedAccessViews(0, 1, &pOutputUAV, nullptr);
    pDeviceContext->CSSetConstantBuffers(0, 1, &pParamBuffer);

    // registra buffer D3D11 con CUDA
    cudaError_t err = cudaGraphicsD3D11RegisterResource(
        &g_cudaResource,
        pGpuFloatOutputBuffer,
        cudaGraphicsRegisterFlagsNone
    );
    if (err != cudaSuccess)
        return false;

    // pre-alloca tensor CUDA una volta sola — zero alloc per frame
    g_cudaResult = torch::zeros(
        { 1, 3, (long long)cropHeight, (long long)cropWidth },
        torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA)
    );

    return true;
}

py::object get_frame_image()
{
    IDXGIResource*          pDesktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ID3D11Texture2D*        pAcquiredTexture = nullptr;

    if (pDeskDupl->AcquireNextFrame(0, &frameInfo, &pDesktopResource) != S_OK)
        return py::none();

    pDesktopResource->QueryInterface(__uuidof(ID3D11Texture2D),
        (void**)&pAcquiredTexture);

    pDesktopResource->Release();

    D3D11_BOX cropBox = {};
    cropBox.left   = cropLeft;
    cropBox.top    = cropTop;
    cropBox.right  = cropLeft + cropWidth;
    cropBox.bottom = cropTop  + cropHeight;
    cropBox.front  = 0;
    cropBox.back   = 1;

    pDeviceContext->CopySubresourceRegion(
        pGpuTargetTexture, 0, 0, 0, 0,
        pAcquiredTexture,  0, &cropBox
    );

    pAcquiredTexture->Release();

    pDeviceContext->Dispatch(
        (cropWidth  + 15) / 16,
        (cropHeight + 15) / 16,
        1
    );

    pDeskDupl->ReleaseFrame();

    pDeviceContext->Flush();

    cudaGraphicsMapResources(1, &g_cudaResource, 0);

    float* d_ptr = nullptr;
    size_t mappedSize = 0;
    cudaGraphicsResourceGetMappedPointer(
        (void**)&d_ptr, &mappedSize, g_cudaResource);

    // copia VRAM->VRAM nel tensor pre-allocato — zero alloc
    cudaMemcpy(
        g_cudaResult.data_ptr(),
        d_ptr,
        cropWidth * cropHeight * 3 * sizeof(float),
        cudaMemcpyDeviceToDevice
    );

    cudaGraphicsUnmapResources(1, &g_cudaResource, 0);

    return py::cast(g_cudaResult);
}

PYBIND11_MODULE(gpu_capture, m) {
    m.def("initialize_capture", &initialize_capture);
    m.def("get_frame_image",    &get_frame_image);
    m.def("cleanup",            &cleanup);
}
