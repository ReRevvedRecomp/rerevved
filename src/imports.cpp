// Title-specific import compatibility for the ReXGlue runtime path.
//
// The pinned ReXGlue SDK catalogs these Xbox USB camera exports, but its
// xboxkrnl_usbcam.cpp translation unit is not part of the SDK build.
// Preserve the title's "camera absent" behavior until the SDK supplies compiled
// implementations that are validated here.

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
