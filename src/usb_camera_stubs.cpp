// The SDK catalogs USB-camera exports but omits xboxkrnl_usbcam.cpp from its
// build. These stubs provide the title's camera-absent behavior.

#include <rex/hook.h>

namespace
{

constexpr unsigned int kErrorDeviceNotConnected = 0x48F;

} // namespace

REX_EXPORT_STUB_RETURN(__imp__XUsbcamCreate, kErrorDeviceNotConnected);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamDestroy, 0);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamGetState, kErrorDeviceNotConnected);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetConfig, kErrorDeviceNotConnected);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetCaptureMode,
                       kErrorDeviceNotConnected);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamReadFrame, kErrorDeviceNotConnected);
