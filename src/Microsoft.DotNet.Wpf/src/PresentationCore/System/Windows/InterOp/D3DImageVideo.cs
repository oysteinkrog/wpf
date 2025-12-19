// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Description: D3DImageVideo class
//              An ImageSource optimized for video rendering with D3D11 support

using MS.Internal;
using MS.Win32.PresentationCore;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows.Media;
using System.Windows.Media.Composition;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using System.Threading;

namespace System.Windows.Interop
{
    /// <summary>
    ///     Specifies the pixel format for video frames.
    /// </summary>
    public enum VideoPixelFormat
    {
        /// <summary>Standard 8-bit BGRA format (no conversion needed)</summary>
        BGRA8 = 0,

        /// <summary>YUV 4:2:0 8-bit format (NV12, typical hardware decode output)</summary>
        NV12 = 1,

        /// <summary>YUV 4:2:0 10-bit format (P010, HDR content)</summary>
        P010 = 2,
    }

    /// <summary>
    ///     An ImageSource optimized for video rendering with D3D11 input support.
    ///
    ///     Key features over D3DImage:
    ///     - Accepts D3D11 textures directly (no app-side D3D11→D3D9 bridge needed)
    ///     - GPU fence synchronization (non-blocking, no Lock/Unlock required)
    ///     - Native YUV support (NV12/P010 converted to RGB internally)
    ///     - Late-latch frame selection (newest completed frame always displayed)
    ///     - Pre-allocated triple buffering (no per-frame allocations)
    /// </summary>
    public class D3DImageVideo : ImageSource, IAppDomainShutdownListener
    {
        #region Static Constructor

        static D3DImageVideo()
        {
            IsFrontBufferAvailablePropertyKey =
                DependencyProperty.RegisterReadOnly(
                    "IsFrontBufferAvailable",
                    typeof(bool),
                    typeof(D3DImageVideo),
                    new UIPropertyMetadata(
                        BooleanBoxes.FalseBox,
                        new PropertyChangedCallback(IsFrontBufferAvailablePropertyChanged)
                        )
                    );

            IsFrontBufferAvailableProperty = IsFrontBufferAvailablePropertyKey.DependencyProperty;
        }

        #endregion

        #region Constructors

        /// <summary>
        ///     Default constructor, uses 96 DPI.
        /// </summary>
        public D3DImageVideo() : this(96.0, 96.0)
        {
        }

        /// <summary>
        ///     Constructor with custom DPI.
        /// </summary>
        public D3DImageVideo(double dpiX, double dpiY)
        {
            ArgumentOutOfRangeException.ThrowIfNegative(dpiX);
            ArgumentOutOfRangeException.ThrowIfNegative(dpiY);

            _availableCallback = OnFrontBufferAvailableCallback;
            _dpiX = dpiX;
            _dpiY = dpiY;

            _listener = new WeakReference<IAppDomainShutdownListener>(this);
            AppDomainShutdownMonitor.Add(_listener);
        }

        ~D3DImageVideo()
        {
            if (_pInteropDeviceBitmap != IntPtr.Zero)
            {
                UnsafeNativeMethods.InteropDeviceBitmap11.Detach(_pInteropDeviceBitmap);
            }

            AppDomainShutdownMonitor.Remove(_listener);
        }

        #endregion

        #region Public Methods

        /// <summary>
        ///     Initialize the video surface with specified dimensions and format.
        ///     Must be called before SubmitFrame.
        /// </summary>
        /// <param name="width">Width in pixels</param>
        /// <param name="height">Height in pixels</param>
        /// <param name="format">Pixel format of input textures</param>
        /// <param name="bufferCount">Number of buffers for triple buffering (2-4, default 3)</param>
        public void Initialize(int width, int height, VideoPixelFormat format, int bufferCount = 3)
        {
            WritePreamble();

            ArgumentOutOfRangeException.ThrowIfNegativeOrZero(width);
            ArgumentOutOfRangeException.ThrowIfNegativeOrZero(height);
            ArgumentOutOfRangeException.ThrowIfLessThan(bufferCount, 2);
            ArgumentOutOfRangeException.ThrowIfGreaterThan(bufferCount, 4);

            if (_isInitialized)
            {
                throw new InvalidOperationException("D3DImageVideo is already initialized. Create a new instance to change dimensions.");
            }

            IntPtr newBitmap = IntPtr.Zero;
            uint actualWidth = 0;
            uint actualHeight = 0;

            HRESULT.Check(UnsafeNativeMethods.InteropDeviceBitmap11.Create(
                (uint)width,
                (uint)height,
                (int)format,
                (uint)bufferCount,
                _dpiX,
                _dpiY,
                ++_version,
                _availableCallback,
                out newBitmap,
                out actualWidth,
                out actualHeight
                ));

            _pInteropDeviceBitmap = newBitmap;
            _pixelWidth = actualWidth;
            _pixelHeight = actualHeight;
            _format = format;
            _isInitialized = true;

            RegisterForAsyncUpdateResource();

            WritePostscript();
        }

        /// <summary>
        ///     Submit a D3D11 texture for display.
        ///
        ///     This is a non-blocking call. The texture will be displayed when:
        ///     1. The fence (if provided) signals completion
        ///     2. WPF's composition cycle picks it up
        ///
        ///     Uses "latest-frame-wins" semantics - if multiple frames are submitted
        ///     before composition, only the newest completed frame is displayed.
        /// </summary>
        /// <param name="d3d11SharedTextureHandle">
        ///     Shared handle to the D3D11 texture (created with D3D11_RESOURCE_MISC_SHARED).
        ///     The texture must match the format and dimensions specified in Initialize().
        /// </param>
        /// <param name="d3d11SharedFenceHandle">
        ///     Optional shared handle to an ID3D11Fence. If provided, the frame won't be
        ///     displayed until the fence value is reached. Pass IntPtr.Zero for immediate
        ///     display (when GPU work is already complete).
        /// </param>
        /// <param name="fenceValue">
        ///     The fence value to wait for. Ignored if fenceHandle is IntPtr.Zero.
        /// </param>
        public void SubmitFrame(IntPtr d3d11SharedTextureHandle, IntPtr d3d11SharedFenceHandle = default, ulong fenceValue = 0)
        {
            WritePreamble();

            if (!_isInitialized)
            {
                throw new InvalidOperationException("D3DImageVideo must be initialized before submitting frames.");
            }

            if (d3d11SharedTextureHandle == IntPtr.Zero)
            {
                throw new ArgumentNullException(nameof(d3d11SharedTextureHandle));
            }

            HRESULT.Check(UnsafeNativeMethods.InteropDeviceBitmap11.SubmitFrame(
                _pInteropDeviceBitmap,
                d3d11SharedTextureHandle,
                d3d11SharedFenceHandle,
                fenceValue
                ));

            _framesSubmitted++;

            // Trigger composition update
            RegisterForAsyncUpdateResource();

            WritePostscript();
        }

        #endregion

        #region Public Properties

        /// <summary>
        ///     Whether the front buffer is available for rendering.
        /// </summary>
        public bool IsFrontBufferAvailable
        {
            get
            {
                return (bool)GetValue(IsFrontBufferAvailableProperty);
            }
        }

        public static readonly DependencyProperty IsFrontBufferAvailableProperty;

        /// <summary>
        ///     Event fired when IsFrontBufferAvailable changes.
        /// </summary>
        public event DependencyPropertyChangedEventHandler IsFrontBufferAvailableChanged
        {
            add
            {
                WritePreamble();
                if (value != null)
                {
                    _isFrontBufferAvailableChangedHandlers += value;
                }
            }
            remove
            {
                WritePreamble();
                if (value != null)
                {
                    _isFrontBufferAvailableChangedHandlers -= value;
                }
            }
        }

        /// <summary>
        ///     Width in pixels.
        /// </summary>
        public int PixelWidth
        {
            get
            {
                ReadPreamble();
                return (int)_pixelWidth;
            }
        }

        /// <summary>
        ///     Height in pixels.
        /// </summary>
        public int PixelHeight
        {
            get
            {
                ReadPreamble();
                return (int)_pixelHeight;
            }
        }

        /// <summary>
        ///     The pixel format of input textures.
        /// </summary>
        public VideoPixelFormat Format
        {
            get
            {
                ReadPreamble();
                return _format;
            }
        }

        /// <summary>
        ///     Number of frames that have been dropped (newer frame arrived
        ///     before older frame was displayed).
        /// </summary>
        public int DroppedFrameCount
        {
            get
            {
                ReadPreamble();
                if (_pInteropDeviceBitmap != IntPtr.Zero)
                {
                    return (int)UnsafeNativeMethods.InteropDeviceBitmap11.GetDroppedFrameCount(_pInteropDeviceBitmap);
                }
                return 0;
            }
        }

        /// <summary>
        ///     Total number of frames submitted.
        /// </summary>
        public long FramesSubmitted
        {
            get
            {
                ReadPreamble();
                return _framesSubmitted;
            }
        }

        #endregion

        #region ImageSource Overrides

        /// <summary>
        ///     Width in DIPs (device-independent pixels).
        /// </summary>
        public sealed override double Width
        {
            get
            {
                ReadPreamble();
                return ImageSource.PixelsToDIPs(_dpiX, (int)_pixelWidth);
            }
        }

        /// <summary>
        ///     Height in DIPs (device-independent pixels).
        /// </summary>
        public sealed override double Height
        {
            get
            {
                ReadPreamble();
                return ImageSource.PixelsToDIPs(_dpiY, (int)_pixelHeight);
            }
        }

        /// <summary>
        ///     No metadata for video surfaces.
        /// </summary>
        public sealed override ImageMetadata Metadata
        {
            get
            {
                ReadPreamble();
                return null;
            }
        }

        #endregion

        #region Freezable Overrides

        protected override Freezable CreateInstanceCore()
        {
            return new D3DImageVideo();
        }

        /// <summary>
        ///     Freezing is not allowed - video surfaces need continuous updates.
        /// </summary>
        protected sealed override bool FreezeCore(bool isChecking)
        {
            return false;
        }

        #endregion

        #region IAppDomainShutdownListener

        void IAppDomainShutdownListener.NotifyShutdown()
        {
            if (_pInteropDeviceBitmap != IntPtr.Zero)
            {
                UnsafeNativeMethods.InteropDeviceBitmap11.Detach(_pInteropDeviceBitmap);
                _pInteropDeviceBitmap = IntPtr.Zero;
            }
        }

        #endregion

        #region Private Methods

        private static void IsFrontBufferAvailablePropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            D3DImageVideo target = (D3DImageVideo)d;
            target._isFrontBufferAvailableChangedHandlers?.Invoke(target, e);
        }

        [UnmanagedCallersOnly]
        private static void OnFrontBufferAvailableCallback(int fAvailable, uint version)
        {
            // This is called from unmanaged code - need to marshal to UI thread
            // For now, use simple approach
        }

        #endregion

        #region Private Fields

        private static readonly DependencyPropertyKey IsFrontBufferAvailablePropertyKey;

        private IntPtr _pInteropDeviceBitmap;
        private uint _pixelWidth;
        private uint _pixelHeight;
        private VideoPixelFormat _format;
        private double _dpiX;
        private double _dpiY;
        private uint _version;
        private bool _isInitialized;
        private long _framesSubmitted;

        private DependencyPropertyChangedEventHandler _isFrontBufferAvailableChangedHandlers;
        private readonly UnsafeNativeMethods.InteropDeviceBitmap11.FrontBufferAvailableCallback _availableCallback;
        private readonly WeakReference<IAppDomainShutdownListener> _listener;

        #endregion
    }
}

namespace MS.Win32.PresentationCore
{
    internal static partial class UnsafeNativeMethods
    {
        internal static class InteropDeviceBitmap11
        {
            internal delegate void FrontBufferAvailableCallback(int fAvailable, uint version);

            [DllImport(DllImport.MilCore, EntryPoint = "InteropDeviceBitmap11_Create")]
            internal static extern int Create(
                uint width,
                uint height,
                int format,
                uint bufferCount,
                double dpiX,
                double dpiY,
                uint version,
                FrontBufferAvailableCallback callback,
                out IntPtr ppBitmap,
                out uint puWidth,
                out uint puHeight
                );

            [DllImport(DllImport.MilCore, EntryPoint = "InteropDeviceBitmap11_SubmitFrame")]
            internal static extern int SubmitFrame(
                IntPtr pBitmap,
                IntPtr hSharedTexture,
                IntPtr hSharedFence,
                ulong fenceValue
                );

            [DllImport(DllImport.MilCore, EntryPoint = "InteropDeviceBitmap11_Detach")]
            internal static extern void Detach(IntPtr pBitmap);

            [DllImport(DllImport.MilCore, EntryPoint = "InteropDeviceBitmap11_GetDroppedFrameCount")]
            internal static extern uint GetDroppedFrameCount(IntPtr pBitmap);
        }
    }
}
