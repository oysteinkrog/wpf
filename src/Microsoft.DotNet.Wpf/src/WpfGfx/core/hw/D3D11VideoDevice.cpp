// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------------
//
//  Class:
//      CD3D11VideoDevice
//
//  Synopsis:
//      Implementation of singleton D3D11 device for video processing.
//
//------------------------------------------------------------------------------

#include "precomp.hpp"
#include "D3D11VideoDevice.h"

#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

MtDefine(CD3D11VideoDevice, MILRender, "CD3D11VideoDevice");

//+-----------------------------------------------------------------------------
//  Static member initialization
//------------------------------------------------------------------------------

CD3D11VideoDevice *CD3D11VideoDevice::s_pInstance = nullptr;
CCriticalSection CD3D11VideoDevice::s_csInstance;
bool CD3D11VideoDevice::s_fInitAttempted = false;
bool CD3D11VideoDevice::s_fInitSucceeded = false;

//+-----------------------------------------------------------------------------
//  YUV to RGB conversion shader (HLSL)
//------------------------------------------------------------------------------

static const char s_szFullScreenVS[] = R"(
struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VS_OUTPUT main(uint vertexId : SV_VertexID)
{
    VS_OUTPUT output;

    // Generate fullscreen triangle from vertex ID
    output.TexCoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(output.TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    return output;
}
)";

static const char s_szNV12ToRgbaPS[] = R"(
Texture2D<float> texY : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState samplerLinear : register(s0);

cbuffer ColorMatrix : register(b0)
{
    float4x4 yuvToRgb;
};

float4 main(float4 position : SV_POSITION, float2 texCoord : TEXCOORD0) : SV_TARGET
{
    float y = texY.Sample(samplerLinear, texCoord);
    float2 uv = texUV.Sample(samplerLinear, texCoord);

    // NV12: Y is 0-1, UV is 0-1 but needs to be centered at 0.5
    float4 yuva = float4(y, uv.x - 0.5, uv.y - 0.5, 1.0);

    float4 rgba = mul(yuvToRgb, yuva);
    rgba.a = 1.0;

    return saturate(rgba);
}
)";

static const char s_szP010ToRgbaPS[] = R"(
Texture2D<float> texY : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState samplerLinear : register(s0);

cbuffer ColorMatrix : register(b0)
{
    float4x4 yuvToRgb;
};

// PQ EOTF for HDR (SMPTE ST 2084)
float3 PQToLinear(float3 pq)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 Np = pow(pq, 1.0 / m2);
    float3 L = pow(max(Np - c1, 0.0) / (c2 - c3 * Np), 1.0 / m1);

    return L * 10000.0; // Returns nits
}

float4 main(float4 position : SV_POSITION, float2 texCoord : TEXCOORD0) : SV_TARGET
{
    // P010 is 10-bit packed in 16-bit, so values are in upper 10 bits
    float y = texY.Sample(samplerLinear, texCoord);
    float2 uv = texUV.Sample(samplerLinear, texCoord);

    float4 yuva = float4(y, uv.x - 0.5, uv.y - 0.5, 1.0);

    float4 rgba = mul(yuvToRgb, yuva);

    // For HDR, we'd apply PQ transfer here, but for now just saturate to SDR
    rgba.rgb = saturate(rgba.rgb);
    rgba.a = 1.0;

    return rgba;
}
)";

// BT.709 YUV to RGB matrix (for HD content)
static const float s_BT709Matrix[16] = {
    1.0f,     1.0f,      1.0f,     0.0f,
    0.0f,    -0.21482f,  2.12798f, 0.0f,
    1.28033f,-0.38059f,  0.0f,     0.0f,
    0.0f,     0.0f,      0.0f,     1.0f
};

//+-----------------------------------------------------------------------------
//  Constructor / Destructor
//------------------------------------------------------------------------------

CD3D11VideoDevice::CD3D11VideoDevice()
    : m_pDevice(nullptr)
    , m_pContext(nullptr)
    , m_pDxgiDevice(nullptr)
    , m_pFullScreenVS(nullptr)
    , m_pNV12ToRgbaPS(nullptr)
    , m_pP010ToRgbaPS(nullptr)
    , m_pLinearSampler(nullptr)
    , m_pColorMatrixCB(nullptr)
    , m_pInputLayout(nullptr)
    , m_pRasterizerState(nullptr)
    , m_fInitialized(false)
{
}

CD3D11VideoDevice::~CD3D11VideoDevice()
{
    ReleaseResources();
}

//+-----------------------------------------------------------------------------
//  Singleton access
//------------------------------------------------------------------------------

HRESULT
CD3D11VideoDevice::GetInstance(
    __deref_out CD3D11VideoDevice **ppDevice
    )
{
    HRESULT hr = S_OK;

    CGuard<CCriticalSection> guard(s_csInstance);

    if (!s_fInitAttempted)
    {
        s_fInitAttempted = true;

        s_pInstance = new CD3D11VideoDevice();
        if (s_pInstance == nullptr)
        {
            hr = E_OUTOFMEMORY;
            goto Cleanup;
        }

        hr = s_pInstance->Initialize();
        if (FAILED(hr))
        {
            delete s_pInstance;
            s_pInstance = nullptr;
            goto Cleanup;
        }

        s_fInitSucceeded = true;
    }

    if (!s_fInitSucceeded)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    *ppDevice = s_pInstance;

Cleanup:
    return hr;
}

void
CD3D11VideoDevice::ReleaseInstance()
{
    CGuard<CCriticalSection> guard(s_csInstance);

    if (s_pInstance != nullptr)
    {
        delete s_pInstance;
        s_pInstance = nullptr;
    }

    s_fInitAttempted = false;
    s_fInitSucceeded = false;
}

bool
CD3D11VideoDevice::IsAvailable()
{
    CGuard<CCriticalSection> guard(s_csInstance);
    return s_fInitSucceeded;
}

//+-----------------------------------------------------------------------------
//  Initialization
//------------------------------------------------------------------------------

HRESULT
CD3D11VideoDevice::Initialize()
{
    HRESULT hr = S_OK;

    // Create D3D11 device with D3D11_CREATE_DEVICE_VIDEO_SUPPORT
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if DBG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    ID3D11Device *pDevice = nullptr;
    ID3D11DeviceContext *pContext = nullptr;
    D3D_FEATURE_LEVEL actualFeatureLevel;

    hr = D3D11CreateDevice(
        nullptr,                    // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // No software rasterizer
        createDeviceFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &pDevice,
        &actualFeatureLevel,
        &pContext
        );

    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // Query for ID3D11Device5 (needed for fences)
    hr = pDevice->QueryInterface(IID_PPV_ARGS(&m_pDevice));
    if (FAILED(hr))
    {
        // Fences require Windows 10, fall back gracefully
        // We can still work without fences using CPU sync
        hr = pDevice->QueryInterface(IID_PPV_ARGS(reinterpret_cast<ID3D11Device**>(&m_pDevice)));
        if (FAILED(hr))
        {
            goto Cleanup;
        }
    }

    hr = pContext->QueryInterface(IID_PPV_ARGS(&m_pContext));
    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // Get DXGI device for shared resource handling
    hr = m_pDevice->QueryInterface(IID_PPV_ARGS(&m_pDxgiDevice));
    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // Create YUV conversion resources
    hr = CreateYuvConversionResources();
    if (FAILED(hr))
    {
        goto Cleanup;
    }

    m_fInitialized = true;

Cleanup:
    ReleaseInterface(pDevice);
    ReleaseInterface(pContext);

    if (FAILED(hr))
    {
        ReleaseResources();
    }

    return hr;
}

HRESULT
CD3D11VideoDevice::CreateYuvConversionResources()
{
    HRESULT hr = S_OK;

    ID3DBlob *pVSBlob = nullptr;
    ID3DBlob *pPSBlobNV12 = nullptr;
    ID3DBlob *pPSBlobP010 = nullptr;
    ID3DBlob *pErrorBlob = nullptr;

    // Declare all structs at top to avoid C4533 (goto skipping initialization)
    D3D11_SAMPLER_DESC samplerDesc;
    D3D11_BUFFER_DESC cbDesc;
    D3D11_SUBRESOURCE_DATA cbData;
    D3D11_RASTERIZER_DESC rastDesc;

    ZeroMemory(&samplerDesc, sizeof(samplerDesc));
    ZeroMemory(&cbDesc, sizeof(cbDesc));
    ZeroMemory(&cbData, sizeof(cbData));
    ZeroMemory(&rastDesc, sizeof(rastDesc));

    // Compile vertex shader
    hr = D3DCompile(
        s_szFullScreenVS,
        strlen(s_szFullScreenVS),
        "FullScreenVS",
        nullptr,
        nullptr,
        "main",
        "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &pVSBlob,
        &pErrorBlob
        );

    if (FAILED(hr))
    {
        goto Cleanup;
    }

    hr = m_pDevice->CreateVertexShader(
        pVSBlob->GetBufferPointer(),
        pVSBlob->GetBufferSize(),
        nullptr,
        &m_pFullScreenVS
        );

    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // Compile NV12 pixel shader
    hr = D3DCompile(
        s_szNV12ToRgbaPS,
        strlen(s_szNV12ToRgbaPS),
        "NV12ToRgbaPS",
        nullptr,
        nullptr,
        "main",
        "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &pPSBlobNV12,
        &pErrorBlob
        );

    if (FAILED(hr))
    {
        goto Cleanup;
    }

    hr = m_pDevice->CreatePixelShader(
        pPSBlobNV12->GetBufferPointer(),
        pPSBlobNV12->GetBufferSize(),
        nullptr,
        &m_pNV12ToRgbaPS
        );

    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // Compile P010 pixel shader
    hr = D3DCompile(
        s_szP010ToRgbaPS,
        strlen(s_szP010ToRgbaPS),
        "P010ToRgbaPS",
        nullptr,
        nullptr,
        "main",
        "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &pPSBlobP010,
        &pErrorBlob
        );

    if (FAILED(hr))
    {
        goto Cleanup;
    }

    hr = m_pDevice->CreatePixelShader(
        pPSBlobP010->GetBufferPointer(),
        pPSBlobP010->GetBufferSize(),
        nullptr,
        &m_pP010ToRgbaPS
        );

    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // Create sampler state
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = m_pDevice->CreateSamplerState(&samplerDesc, &m_pLinearSampler);
    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // Create constant buffer for color matrix
    cbDesc.ByteWidth = sizeof(s_BT709Matrix);
    cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    cbData.pSysMem = s_BT709Matrix;

    hr = m_pDevice->CreateBuffer(&cbDesc, &cbData, &m_pColorMatrixCB);
    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // Create rasterizer state
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;

    hr = m_pDevice->CreateRasterizerState(&rastDesc, &m_pRasterizerState);
    if (FAILED(hr))
    {
        goto Cleanup;
    }

Cleanup:
    ReleaseInterface(pVSBlob);
    ReleaseInterface(pPSBlobNV12);
    ReleaseInterface(pPSBlobP010);
    ReleaseInterface(pErrorBlob);

    return hr;
}

void
CD3D11VideoDevice::ReleaseResources()
{
    ReleaseInterface(m_pRasterizerState);
    ReleaseInterface(m_pInputLayout);
    ReleaseInterface(m_pColorMatrixCB);
    ReleaseInterface(m_pLinearSampler);
    ReleaseInterface(m_pP010ToRgbaPS);
    ReleaseInterface(m_pNV12ToRgbaPS);
    ReleaseInterface(m_pFullScreenVS);
    ReleaseInterface(m_pDxgiDevice);
    ReleaseInterface(m_pContext);
    ReleaseInterface(m_pDevice);

    m_fInitialized = false;
}

//+-----------------------------------------------------------------------------
//  Shared texture operations
//------------------------------------------------------------------------------

HRESULT
CD3D11VideoDevice::OpenSharedTexture(
    __in HANDLE hSharedHandle,
    __deref_out ID3D11Texture2D **ppTexture
    )
{
    HRESULT hr = S_OK;

    if (!m_fInitialized)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    hr = m_pDevice->OpenSharedResource(
        hSharedHandle,
        IID_PPV_ARGS(ppTexture)
        );

Cleanup:
    return hr;
}

HRESULT
CD3D11VideoDevice::OpenSharedFence(
    __in HANDLE hSharedHandle,
    __deref_out ID3D11Fence **ppFence
    )
{
    HRESULT hr = S_OK;

    if (!m_fInitialized || m_pDevice == nullptr)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    // ID3D11Device5::OpenSharedFence
    hr = m_pDevice->OpenSharedFence(
        hSharedHandle,
        IID_PPV_ARGS(ppFence)
        );

Cleanup:
    return hr;
}

HRESULT
CD3D11VideoDevice::IsFenceComplete(
    __in ID3D11Fence *pFence,
    __in UINT64 fenceValue,
    __out bool *pIsComplete
    )
{
    HRESULT hr = S_OK;

    *pIsComplete = false;

    if (pFence == nullptr)
    {
        // No fence means immediately complete
        *pIsComplete = true;
        goto Cleanup;
    }

    UINT64 completedValue = pFence->GetCompletedValue();
    *pIsComplete = (completedValue >= fenceValue);

Cleanup:
    return hr;
}

HRESULT
CD3D11VideoDevice::CreateSharedBgraTexture(
    __in UINT width,
    __in UINT height,
    __deref_out ID3D11Texture2D **ppTexture,
    __out HANDLE *phSharedHandle
    )
{
    HRESULT hr = S_OK;
    D3D11_TEXTURE2D_DESC desc;

    ZeroMemory(&desc, sizeof(desc));

    if (!m_fInitialized)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    hr = m_pDevice->CreateTexture2D(&desc, nullptr, ppTexture);
    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // Get the shared handle
    IDXGIResource *pDxgiResource = nullptr;
    hr = (*ppTexture)->QueryInterface(IID_PPV_ARGS(&pDxgiResource));
    if (SUCCEEDED(hr))
    {
        hr = pDxgiResource->GetSharedHandle(phSharedHandle);
        pDxgiResource->Release();
    }

Cleanup:
    return hr;
}

//+-----------------------------------------------------------------------------
//  YUV conversion
//------------------------------------------------------------------------------

HRESULT
CD3D11VideoDevice::ConvertNV12ToRgba(
    __in ID3D11Texture2D *pNV12Texture,
    __in ID3D11Texture2D *pRgbaTexture
    )
{
    HRESULT hr = S_OK;
    D3D11_TEXTURE2D_DESC nv12Desc, rgbaDesc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    D3D11_VIEWPORT viewport;
    ID3D11ShaderResourceView *pYSRV = nullptr;
    ID3D11ShaderResourceView *pUVSRV = nullptr;
    ID3D11RenderTargetView *pRTV = nullptr;

    ZeroMemory(&srvDesc, sizeof(srvDesc));
    ZeroMemory(&viewport, sizeof(viewport));

    if (!m_fInitialized)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    // Get texture descriptions
    pNV12Texture->GetDesc(&nv12Desc);
    pRgbaTexture->GetDesc(&rgbaDesc);

    // Y plane SRV
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = m_pDevice->CreateShaderResourceView(pNV12Texture, &srvDesc, &pYSRV);
    if (FAILED(hr))
    {
        goto Cleanup;
    }

    // UV plane SRV (NV12 has interleaved UV at half resolution)
    srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = m_pDevice->CreateShaderResourceView(pNV12Texture, &srvDesc, &pUVSRV);
    if (FAILED(hr))
    {
        ReleaseInterface(pYSRV);
        goto Cleanup;
    }

    // RTV for output
    hr = m_pDevice->CreateRenderTargetView(pRgbaTexture, nullptr, &pRTV);
    if (FAILED(hr))
    {
        ReleaseInterface(pYSRV);
        ReleaseInterface(pUVSRV);
        goto Cleanup;
    }

    // Set up pipeline
    m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_pContext->IASetInputLayout(nullptr);  // No vertex buffer needed

    m_pContext->VSSetShader(m_pFullScreenVS, nullptr, 0);
    m_pContext->PSSetShader(m_pNV12ToRgbaPS, nullptr, 0);

    ID3D11ShaderResourceView *srvs[] = { pYSRV, pUVSRV };
    m_pContext->PSSetShaderResources(0, 2, srvs);
    m_pContext->PSSetSamplers(0, 1, &m_pLinearSampler);
    m_pContext->PSSetConstantBuffers(0, 1, &m_pColorMatrixCB);

    m_pContext->RSSetState(m_pRasterizerState);

    viewport.Width = static_cast<float>(rgbaDesc.Width);
    viewport.Height = static_cast<float>(rgbaDesc.Height);
    viewport.MaxDepth = 1.0f;
    m_pContext->RSSetViewports(1, &viewport);

    m_pContext->OMSetRenderTargets(1, &pRTV, nullptr);

    // Draw fullscreen triangle
    m_pContext->Draw(3, 0);

    // Cleanup
    ID3D11ShaderResourceView *nullSRVs[] = { nullptr, nullptr };
    m_pContext->PSSetShaderResources(0, 2, nullSRVs);

    ID3D11RenderTargetView *nullRTV = nullptr;
    m_pContext->OMSetRenderTargets(1, &nullRTV, nullptr);

    ReleaseInterface(pYSRV);
    ReleaseInterface(pUVSRV);
    ReleaseInterface(pRTV);

Cleanup:
    return hr;
}

HRESULT
CD3D11VideoDevice::ConvertP010ToRgba(
    __in ID3D11Texture2D *pP010Texture,
    __in ID3D11Texture2D *pRgbaTexture
    )
{
    // Similar to NV12 but with 16-bit formats
    // For now, use same logic as NV12 (P010 is 10-bit in 16-bit container)
    return ConvertNV12ToRgba(pP010Texture, pRgbaTexture);
}

HRESULT
CD3D11VideoDevice::CopyTexture(
    __in ID3D11Texture2D *pSrcTexture,
    __in ID3D11Texture2D *pDstTexture
    )
{
    HRESULT hr = S_OK;

    if (!m_fInitialized)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    m_pContext->CopyResource(pDstTexture, pSrcTexture);

Cleanup:
    return hr;
}

void
CD3D11VideoDevice::Flush()
{
    if (m_fInitialized && m_pContext != nullptr)
    {
        m_pContext->Flush();
    }
}
