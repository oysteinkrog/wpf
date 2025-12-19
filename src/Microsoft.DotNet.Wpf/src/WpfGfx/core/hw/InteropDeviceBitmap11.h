// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------------
//
//  Class:
//      CInteropDeviceBitmap11
//
//  Synopsis:
//      D3D11-based interop bitmap for video rendering with GPU fence support.
//      Accepts D3D11 textures from the application, performs YUV→RGB conversion
//      if needed, and provides D3D9 surfaces for MIL composition.
//
//      Key features:
//      - Late-latch frame selection (newest completed frame wins)
//      - GPU fence synchronization (non-blocking)
//      - Native YUV support (NV12, P010)
//      - Triple buffering with pre-allocated surfaces
//
//------------------------------------------------------------------------------

#pragma once

#include "D3D11VideoDevice.h"

MtExtern(CInteropDeviceBitmap11);

// Video pixel format enum matching managed side
enum class VideoPixelFormat
{
    BGRA8 = 0,      // B8G8R8A8_UNorm (standard, no conversion needed)
    NV12 = 1,       // YUV 4:2:0 8-bit (hardware decode)
    P010 = 2,       // YUV 4:2:0 10-bit (HDR)
};

// Callback for front buffer availability changes
typedef void (STDMETHODCALLTYPE *FrontBufferAvailableCallback11)(
    BOOL fAvailable,
    UINT uVersion
    );

class CInteropDeviceBitmap11 : public CDeviceBitmap
{
public:
    DECLARE_METERHEAP_ALLOC(ProcessHeap, Mt(CInteropDeviceBitmap11));

    //+-------------------------------------------------------------------------
    //  Exported functions (called from managed code via P/Invoke)
    //--------------------------------------------------------------------------
    static HRESULT Create(
        __in UINT width,
        __in UINT height,
        __in VideoPixelFormat format,
        __in UINT bufferCount,
        __in_range(0, DBL_MAX) double dpiX,
        __in_range(0, DBL_MAX) double dpiY,
        __in UINT uVersion,
        __in FrontBufferAvailableCallback11 pfnAvailable,
        __deref_out CInteropDeviceBitmap11 **ppBitmap
        );

    //+-------------------------------------------------------------------------
    //  IUnknown implementation
    //--------------------------------------------------------------------------
    STDMETHOD(QueryInterface)(REFIID riid, __deref_out void **ppvObject) override;

    STDMETHOD_(ULONG, AddRef)() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHOD_(ULONG, Release)() override
    {
        ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0)
        {
            delete this;
        }
        return cRef;
    }

    //+-------------------------------------------------------------------------
    //  Frame submission (called from UI thread)
    //--------------------------------------------------------------------------

    // Submit a D3D11 texture for display
    // hSharedTexture: Shared handle to app's D3D11 texture
    // hSharedFence: Optional shared handle to ID3D11Fence (NULL for immediate)
    // fenceValue: Fence value to wait for (ignored if hSharedFence is NULL)
    HRESULT SubmitFrame(
        __in HANDLE hSharedTexture,
        __in_opt HANDLE hSharedFence,
        __in UINT64 fenceValue
        );

    // Detach from managed D3DImageVideo
    void Detach();

    //+-------------------------------------------------------------------------
    //  Composition interface (called from render thread)
    //--------------------------------------------------------------------------

    // Get the D3D9 surface for MIL composition
    // Uses late-latch to select newest completed frame
    HRESULT GetD3D9SurfaceForComposition(
        __deref_out IDirect3DSurface9 **ppSurface
        );

    // Check if there are pending frames to display
    bool HasPendingFrames() const;

    // Get diagnostic info
    UINT GetDroppedFrameCount() const { return m_droppedFrameCount; }

    //+-------------------------------------------------------------------------
    //  CDeviceBitmap overrides
    //--------------------------------------------------------------------------
    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }

private:
    CInteropDeviceBitmap11(
        __in UINT width,
        __in UINT height,
        __in VideoPixelFormat format,
        __in UINT bufferCount,
        __in UINT uVersion,
        __in FrontBufferAvailableCallback11 pfnAvailable
        );

    ~CInteropDeviceBitmap11();

    HRESULT Initialize(double dpiX, double dpiY);
    HRESULT CreateSharedSurfacePool();
    void ReleaseResources();

    // Find the newest frame slot where the fence is complete
    int FindNewestReadyFrame();

    // Process a frame slot (open shared texture, convert YUV, etc.)
    HRESULT ProcessFrame(int slot);

    //+-------------------------------------------------------------------------
    //  Frame slot structure for triple buffering
    //--------------------------------------------------------------------------
    static const int MAX_SLOTS = 4;

    struct FrameSlot
    {
        // Input from app (via shared handles)
        HANDLE hSharedTexture;          // Shared handle to app's texture
        HANDLE hSharedFence;            // Shared handle to app's fence
        UINT64 fenceValue;              // Fence value to wait for

        // WPF-side resources
        ID3D11Texture2D *pAppTexture;   // Opened from shared handle
        ID3D11Fence *pFence;            // Opened from shared handle
        ID3D11Texture2D *pRgbaTexture;  // After YUV conversion (owned by WPF)
        HANDLE hRgbaSharedHandle;       // Shared handle for D3D9

        // D3D9 resources for MIL
        IDirect3DTexture9 *pD3D9Texture;
        IDirect3DSurface9 *pD3D9Surface;

        // State
        UINT64 submitSequence;          // Monotonic submission order
        bool ready;                     // Frame is ready for display

        void Clear()
        {
            hSharedTexture = NULL;
            hSharedFence = NULL;
            fenceValue = 0;
            pAppTexture = nullptr;
            pFence = nullptr;
            pRgbaTexture = nullptr;
            hRgbaSharedHandle = NULL;
            pD3D9Texture = nullptr;
            pD3D9Surface = nullptr;
            submitSequence = 0;
            ready = false;
        }
    };

    //+-------------------------------------------------------------------------
    //  Members
    //--------------------------------------------------------------------------

    // Reference counting
    LONG m_cRef;

    // Dimensions and format
    UINT m_width;
    UINT m_height;
    VideoPixelFormat m_format;
    UINT m_bufferCount;

    // Callback
    UINT m_uVersion;
    FrontBufferAvailableCallback11 m_pfnAvailable;

    // D3D11 video device (singleton)
    CD3D11VideoDevice *m_pVideoDevice;

    // D3D9 device for sharing to MIL
    IDirect3DDevice9Ex *m_pD3D9Device;

    // Frame slots
    FrameSlot m_slots[MAX_SLOTS];

    // Slot tracking
    volatile LONG m_nextSlotIndex;      // Next slot to use for submission
    volatile LONG m_newestSubmittedSlot;// Most recently submitted slot
    int m_currentDisplaySlot;           // Currently displayed slot
    UINT64 m_submitSequence;            // Monotonic submission counter

    // Diagnostics
    UINT m_droppedFrameCount;

    // Synchronization
    CCriticalSection m_cs;

    // State
    bool m_fInitialized;
    bool m_fDetached;
};

//+-----------------------------------------------------------------------------
//  Exported functions for P/Invoke
//------------------------------------------------------------------------------

extern "C"
{
    HRESULT WINAPI InteropDeviceBitmap11_Create(
        __in UINT width,
        __in UINT height,
        __in int format,    // VideoPixelFormat cast to int
        __in UINT bufferCount,
        __in_range(0, DBL_MAX) double dpiX,
        __in_range(0, DBL_MAX) double dpiY,
        __in UINT uVersion,
        __in FrontBufferAvailableCallback11 pfnAvailable,
        __deref_out CInteropDeviceBitmap11 **ppBitmap,
        __out UINT *puWidth,
        __out UINT *puHeight
        );

    HRESULT WINAPI InteropDeviceBitmap11_SubmitFrame(
        __in CInteropDeviceBitmap11 *pBitmap,
        __in HANDLE hSharedTexture,
        __in_opt HANDLE hSharedFence,
        __in UINT64 fenceValue
        );

    void WINAPI InteropDeviceBitmap11_Detach(
        __in CInteropDeviceBitmap11 *pBitmap
        );

    UINT WINAPI InteropDeviceBitmap11_GetDroppedFrameCount(
        __in CInteropDeviceBitmap11 *pBitmap
        );
}
