// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------------
//
//  Class:
//      CInteropDeviceBitmap11
//
//  Synopsis:
//      Implementation of D3D11-based interop bitmap for video rendering.
//
//------------------------------------------------------------------------------

#include "precomp.hpp"
#include "InteropDeviceBitmap11.h"

MtDefine(CInteropDeviceBitmap11, MILRender, "CInteropDeviceBitmap11");

//+-----------------------------------------------------------------------------
//  Exported Functions
//------------------------------------------------------------------------------

HRESULT WINAPI
InteropDeviceBitmap11_Create(
    __in UINT width,
    __in UINT height,
    __in int format,
    __in UINT bufferCount,
    __in_range(0, DBL_MAX) double dpiX,
    __in_range(0, DBL_MAX) double dpiY,
    __in UINT uVersion,
    __in FrontBufferAvailableCallback11 pfnAvailable,
    __deref_out CInteropDeviceBitmap11 **ppBitmap,
    __out UINT *puWidth,
    __out UINT *puHeight
    )
{
    HRESULT hr = S_OK;

    CHECKPTR(pfnAvailable);
    CHECKPTR(ppBitmap);
    CHECKPTR(puWidth);
    CHECKPTR(puHeight);

    if (width == 0 || height == 0 || width > SURFACE_RECT_MAX || height > SURFACE_RECT_MAX)
    {
        IFC(E_INVALIDARG);
    }

    if (bufferCount < 2 || bufferCount > 4)
    {
        IFC(E_INVALIDARG);
    }

    if (dpiX < 0 || dpiY < 0)
    {
        IFC(E_INVALIDARG);
    }

    IFC(CInteropDeviceBitmap11::Create(
        width,
        height,
        static_cast<VideoPixelFormat>(format),
        bufferCount,
        dpiX,
        dpiY,
        uVersion,
        pfnAvailable,
        ppBitmap
        ));

    *puWidth = width;
    *puHeight = height;

Cleanup:
    RRETURN(hr);
}

HRESULT WINAPI
InteropDeviceBitmap11_SubmitFrame(
    __in CInteropDeviceBitmap11 *pBitmap,
    __in HANDLE hSharedTexture,
    __in_opt HANDLE hSharedFence,
    __in UINT64 fenceValue
    )
{
    HRESULT hr = S_OK;

    CHECKPTR(pBitmap);
    CHECKPTR(hSharedTexture);

    IFC(pBitmap->SubmitFrame(hSharedTexture, hSharedFence, fenceValue));

Cleanup:
    RRETURN(hr);
}

void WINAPI
InteropDeviceBitmap11_Detach(
    __in CInteropDeviceBitmap11 *pBitmap
    )
{
    if (pBitmap)
    {
        pBitmap->Detach();
    }
}

UINT WINAPI
InteropDeviceBitmap11_GetDroppedFrameCount(
    __in CInteropDeviceBitmap11 *pBitmap
    )
{
    return pBitmap ? pBitmap->GetDroppedFrameCount() : 0;
}

//+-----------------------------------------------------------------------------
//  CInteropDeviceBitmap11 Implementation
//------------------------------------------------------------------------------

HRESULT
CInteropDeviceBitmap11::Create(
    __in UINT width,
    __in UINT height,
    __in VideoPixelFormat format,
    __in UINT bufferCount,
    __in_range(0, DBL_MAX) double dpiX,
    __in_range(0, DBL_MAX) double dpiY,
    __in UINT uVersion,
    __in FrontBufferAvailableCallback11 pfnAvailable,
    __deref_out CInteropDeviceBitmap11 **ppBitmap
    )
{
    HRESULT hr = S_OK;

    CInteropDeviceBitmap11 *pBitmap = nullptr;

    pBitmap = new CInteropDeviceBitmap11(
        width,
        height,
        format,
        bufferCount,
        uVersion,
        pfnAvailable
        );
    IFCOOM(pBitmap);
    pBitmap->AddRef();

    IFC(pBitmap->Initialize(dpiX, dpiY));

    *ppBitmap = pBitmap;
    pBitmap = nullptr;

Cleanup:
    ReleaseInterface(pBitmap);

    RRETURN(hr);
}

CInteropDeviceBitmap11::CInteropDeviceBitmap11(
    __in UINT width,
    __in UINT height,
    __in VideoPixelFormat format,
    __in UINT bufferCount,
    __in UINT uVersion,
    __in FrontBufferAvailableCallback11 pfnAvailable
    )
    : CDeviceBitmap(width, height, MilPixelFormat::PBGRA32bpp)
    , m_cRef(0)
    , m_width(width)
    , m_height(height)
    , m_format(format)
    , m_bufferCount(bufferCount)
    , m_uVersion(uVersion)
    , m_pfnAvailable(pfnAvailable)
    , m_pVideoDevice(nullptr)
    , m_pD3D9Device(nullptr)
    , m_nextSlotIndex(0)
    , m_newestSubmittedSlot(-1)
    , m_currentDisplaySlot(-1)
    , m_submitSequence(0)
    , m_droppedFrameCount(0)
    , m_fInitialized(false)
    , m_fDetached(false)
{
    for (int i = 0; i < MAX_SLOTS; i++)
    {
        m_slots[i].Clear();
    }
}

CInteropDeviceBitmap11::~CInteropDeviceBitmap11()
{
    ReleaseResources();
    m_cs.DeInit();
}

STDMETHODIMP
CInteropDeviceBitmap11::QueryInterface(REFIID riid, __deref_out void **ppvObject)
{
    if (ppvObject == nullptr)
    {
        return E_POINTER;
    }

    *ppvObject = nullptr;

    if (riid == IID_IUnknown)
    {
        *ppvObject = static_cast<IUnknown*>(static_cast<CMILCOMBase*>(this));
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

HRESULT
CInteropDeviceBitmap11::Initialize(double dpiX, double dpiY)
{
    HRESULT hr = S_OK;
    IDirect3D9Ex *pD3D9Ex = nullptr;

    // Declare all local structs before any IFC to avoid C4533
    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = GetDesktopWindow();
    pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IFC(m_cs.Init());

    // Get the D3D11 video device singleton
    IFC(CD3D11VideoDevice::GetInstance(&m_pVideoDevice));

    // Create D3D9Ex device for sharing surfaces to MIL
    IFC(Direct3DCreate9Ex(D3D_SDK_VERSION, &pD3D9Ex));

    IFC(pD3D9Ex->CreateDeviceEx(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        GetDesktopWindow(),
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
        &pp,
        nullptr,
        &m_pD3D9Device
        ));

    ReleaseInterface(pD3D9Ex);

    // Create pre-allocated shared surface pool
    IFC(CreateSharedSurfacePool());

    IFC(SetResolution(dpiX, dpiY));

    m_fInitialized = true;

    // Notify that front buffer is available
    if (m_pfnAvailable)
    {
        (*m_pfnAvailable)(TRUE, m_uVersion);
    }

Cleanup:
    if (FAILED(hr))
    {
        ReleaseResources();

        if (m_pfnAvailable)
        {
            (*m_pfnAvailable)(FALSE, m_uVersion);
        }
    }

    RRETURN(hr);
}

HRESULT
CInteropDeviceBitmap11::CreateSharedSurfacePool()
{
    HRESULT hr = S_OK;

    // Create pre-allocated RGBA textures that can be shared with D3D9
    for (UINT i = 0; i < m_bufferCount && i < MAX_SLOTS; i++)
    {
        FrameSlot &slot = m_slots[i];

        // Create D3D11 BGRA texture with shared flag
        IFC(m_pVideoDevice->CreateSharedBgraTexture(
            m_width,
            m_height,
            &slot.pRgbaTexture,
            &slot.hRgbaSharedHandle
            ));

        // Create corresponding D3D9 texture from shared handle
        IFC(m_pD3D9Device->CreateTexture(
            m_width,
            m_height,
            1,
            D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,
            &slot.pD3D9Texture,
            &slot.hRgbaSharedHandle
            ));

        IFC(slot.pD3D9Texture->GetSurfaceLevel(0, &slot.pD3D9Surface));
    }

Cleanup:
    RRETURN(hr);
}

void
CInteropDeviceBitmap11::ReleaseResources()
{
    for (int i = 0; i < MAX_SLOTS; i++)
    {
        FrameSlot &slot = m_slots[i];

        ReleaseInterface(slot.pD3D9Surface);
        ReleaseInterface(slot.pD3D9Texture);
        ReleaseInterface(slot.pRgbaTexture);
        ReleaseInterface(slot.pFence);
        ReleaseInterface(slot.pAppTexture);

        slot.Clear();
    }

    ReleaseInterface(m_pD3D9Device);

    // Don't release m_pVideoDevice - it's a singleton
    m_pVideoDevice = nullptr;

    m_fInitialized = false;
}

HRESULT
CInteropDeviceBitmap11::SubmitFrame(
    __in HANDLE hSharedTexture,
    __in_opt HANDLE hSharedFence,
    __in UINT64 fenceValue
    )
{
    HRESULT hr = S_OK;

    CGuard<CCriticalSection> guard(m_cs);

    if (m_fDetached || !m_fInitialized)
    {
        IFC(E_FAIL);
    }

    // Get next slot (round-robin)
    int slotIndex = InterlockedIncrement(&m_nextSlotIndex) % m_bufferCount;
    FrameSlot &slot = m_slots[slotIndex];

    // Release previous app texture/fence for this slot
    ReleaseInterface(slot.pAppTexture);
    ReleaseInterface(slot.pFence);

    // Store new shared handles
    slot.hSharedTexture = hSharedTexture;
    slot.hSharedFence = hSharedFence;
    slot.fenceValue = fenceValue;
    slot.submitSequence = ++m_submitSequence;
    slot.ready = false;

    // Track newest submitted slot
    InterlockedExchange(&m_newestSubmittedSlot, slotIndex);

Cleanup:
    RRETURN(hr);
}

void
CInteropDeviceBitmap11::Detach()
{
    CGuard<CCriticalSection> guard(m_cs);

    m_fDetached = true;
    m_pfnAvailable = nullptr;
}

int
CInteropDeviceBitmap11::FindNewestReadyFrame()
{
    // Find the slot with highest submitSequence where fence is complete
    int bestSlot = -1;
    UINT64 bestSequence = 0;

    for (UINT i = 0; i < m_bufferCount && i < MAX_SLOTS; i++)
    {
        FrameSlot &slot = m_slots[i];

        if (slot.submitSequence == 0)
        {
            continue; // Slot never used
        }

        bool fenceComplete = false;

        if (slot.hSharedFence == NULL)
        {
            // No fence means immediately ready
            fenceComplete = true;
        }
        else if (slot.pFence)
        {
            // Check if fence is complete
            m_pVideoDevice->IsFenceComplete(slot.pFence, slot.fenceValue, &fenceComplete);
        }
        else
        {
            // Need to open fence first
            HRESULT hr = m_pVideoDevice->OpenSharedFence(slot.hSharedFence, &slot.pFence);
            if (SUCCEEDED(hr) && slot.pFence)
            {
                m_pVideoDevice->IsFenceComplete(slot.pFence, slot.fenceValue, &fenceComplete);
            }
        }

        if (fenceComplete && slot.submitSequence > bestSequence)
        {
            bestSlot = i;
            bestSequence = slot.submitSequence;
        }
    }

    // Count dropped frames (any slot with sequence > current display but < best)
    if (bestSlot >= 0 && m_currentDisplaySlot >= 0)
    {
        UINT64 currentSeq = m_slots[m_currentDisplaySlot].submitSequence;
        for (UINT i = 0; i < m_bufferCount && i < MAX_SLOTS; i++)
        {
            FrameSlot &slot = m_slots[i];
            if (slot.submitSequence > currentSeq && slot.submitSequence < bestSequence)
            {
                m_droppedFrameCount++;
            }
        }
    }

    return bestSlot;
}

HRESULT
CInteropDeviceBitmap11::ProcessFrame(int slotIndex)
{
    HRESULT hr = S_OK;

    if (slotIndex < 0 || slotIndex >= MAX_SLOTS)
    {
        IFC(E_INVALIDARG);
    }

    FrameSlot &slot = m_slots[slotIndex];

    // Open app's shared texture if not already done
    if (slot.pAppTexture == nullptr && slot.hSharedTexture != NULL)
    {
        IFC(m_pVideoDevice->OpenSharedTexture(slot.hSharedTexture, &slot.pAppTexture));
    }

    if (slot.pAppTexture == nullptr)
    {
        IFC(E_FAIL);
    }

    // Convert or copy to RGBA texture
    switch (m_format)
    {
        case VideoPixelFormat::BGRA8:
            // Direct copy, no conversion needed
            IFC(m_pVideoDevice->CopyTexture(slot.pAppTexture, slot.pRgbaTexture));
            break;

        case VideoPixelFormat::NV12:
            IFC(m_pVideoDevice->ConvertNV12ToRgba(slot.pAppTexture, slot.pRgbaTexture));
            break;

        case VideoPixelFormat::P010:
            IFC(m_pVideoDevice->ConvertP010ToRgba(slot.pAppTexture, slot.pRgbaTexture));
            break;

        default:
            IFC(E_INVALIDARG);
    }

    // Flush to ensure D3D11 work is done before D3D9 reads
    m_pVideoDevice->Flush();

    slot.ready = true;

Cleanup:
    RRETURN(hr);
}

HRESULT
CInteropDeviceBitmap11::GetD3D9SurfaceForComposition(
    __deref_out IDirect3DSurface9 **ppSurface
    )
{
    HRESULT hr = S_OK;

    CGuard<CCriticalSection> guard(m_cs);

    *ppSurface = nullptr;

    if (!m_fInitialized || m_fDetached)
    {
        IFC(E_FAIL);
    }

    // Late-latch: find newest frame where fence is complete
    int bestSlot = FindNewestReadyFrame();

    if (bestSlot >= 0)
    {
        // Process the frame if it's different from current
        if (bestSlot != m_currentDisplaySlot || !m_slots[bestSlot].ready)
        {
            IFC(ProcessFrame(bestSlot));
            m_currentDisplaySlot = bestSlot;
        }
    }

    // Return current display surface (or first valid one if none selected yet)
    if (m_currentDisplaySlot >= 0 && m_slots[m_currentDisplaySlot].pD3D9Surface)
    {
        *ppSurface = m_slots[m_currentDisplaySlot].pD3D9Surface;
        (*ppSurface)->AddRef();
    }
    else
    {
        // No frame ready yet, return first slot's surface (will be blank)
        if (m_slots[0].pD3D9Surface)
        {
            *ppSurface = m_slots[0].pD3D9Surface;
            (*ppSurface)->AddRef();
        }
    }

Cleanup:
    RRETURN(hr);
}

bool
CInteropDeviceBitmap11::HasPendingFrames() const
{
    return m_newestSubmittedSlot >= 0 && m_newestSubmittedSlot != m_currentDisplaySlot;
}
