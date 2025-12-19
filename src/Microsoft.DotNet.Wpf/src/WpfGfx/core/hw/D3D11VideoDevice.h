// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------------
//
//  Class:
//      CD3D11VideoDevice
//
//  Synopsis:
//      Singleton D3D11 device owned by WPF for video processing.
//      Provides D3D11 capabilities while keeping MIL as D3D9-based.
//
//      Key responsibilities:
//      - Lazy D3D11 device creation on first use
//      - Opening shared textures from app's D3D11 device
//      - YUV→RGBA conversion using pixel shaders
//      - Creating D3D9 surfaces shared with D3D11 textures
//
//------------------------------------------------------------------------------

#pragma once

#include <d3d11_4.h>
#include <dxgi1_2.h>

MtExtern(CD3D11VideoDevice);

class CD3D11VideoDevice
{
public:
    //+-------------------------------------------------------------------------
    //  Singleton access
    //--------------------------------------------------------------------------
    static HRESULT GetInstance(
        __deref_out CD3D11VideoDevice **ppDevice
        );

    static void ReleaseInstance();

    static bool IsAvailable();

    //+-------------------------------------------------------------------------
    //  Device operations
    //--------------------------------------------------------------------------

    // Open a shared texture from another D3D11 device
    HRESULT OpenSharedTexture(
        __in HANDLE hSharedHandle,
        __deref_out ID3D11Texture2D **ppTexture
        );

    // Open a shared fence from another D3D11 device
    HRESULT OpenSharedFence(
        __in HANDLE hSharedHandle,
        __deref_out ID3D11Fence **ppFence
        );

    // Check if a fence value has been reached
    HRESULT IsFenceComplete(
        __in ID3D11Fence *pFence,
        __in UINT64 fenceValue,
        __out bool *pIsComplete
        );

    // Create a BGRA texture that can be shared with D3D9
    HRESULT CreateSharedBgraTexture(
        __in UINT width,
        __in UINT height,
        __deref_out ID3D11Texture2D **ppTexture,
        __out HANDLE *phSharedHandle
        );

    // Convert NV12 texture to BGRA
    HRESULT ConvertNV12ToRgba(
        __in ID3D11Texture2D *pNV12Texture,
        __in ID3D11Texture2D *pRgbaTexture
        );

    // Convert P010 (10-bit HDR) texture to BGRA
    HRESULT ConvertP010ToRgba(
        __in ID3D11Texture2D *pP010Texture,
        __in ID3D11Texture2D *pRgbaTexture
        );

    // Copy texture (for BGRA input that doesn't need conversion)
    HRESULT CopyTexture(
        __in ID3D11Texture2D *pSrcTexture,
        __in ID3D11Texture2D *pDstTexture
        );

    // Flush pending GPU commands
    void Flush();

    // Get the D3D11 device (for advanced use cases)
    ID3D11Device5* GetDevice() const { return m_pDevice; }

private:
    CD3D11VideoDevice();
    ~CD3D11VideoDevice();

    // Non-copyable
    CD3D11VideoDevice(const CD3D11VideoDevice&) = delete;
    CD3D11VideoDevice& operator=(const CD3D11VideoDevice&) = delete;

    HRESULT Initialize();
    HRESULT CreateYuvConversionResources();
    void ReleaseResources();

    //+-------------------------------------------------------------------------
    //  Static singleton instance
    //--------------------------------------------------------------------------
    static CD3D11VideoDevice *s_pInstance;
    static CCriticalSection s_csInstance;
    static bool s_fInitAttempted;
    static bool s_fInitSucceeded;

    //+-------------------------------------------------------------------------
    //  D3D11 resources
    //--------------------------------------------------------------------------
    ID3D11Device5 *m_pDevice;
    ID3D11DeviceContext4 *m_pContext;
    IDXGIDevice2 *m_pDxgiDevice;

    //+-------------------------------------------------------------------------
    //  YUV conversion resources
    //--------------------------------------------------------------------------
    ID3D11VertexShader *m_pFullScreenVS;
    ID3D11PixelShader *m_pNV12ToRgbaPS;
    ID3D11PixelShader *m_pP010ToRgbaPS;
    ID3D11SamplerState *m_pLinearSampler;
    ID3D11Buffer *m_pColorMatrixCB;  // Constant buffer for YUV→RGB matrix
    ID3D11InputLayout *m_pInputLayout;
    ID3D11RasterizerState *m_pRasterizerState;

    //+-------------------------------------------------------------------------
    //  State tracking
    //--------------------------------------------------------------------------
    bool m_fInitialized;
};
