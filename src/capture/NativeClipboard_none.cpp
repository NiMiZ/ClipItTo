#include "NativeClipboard.h"

namespace clipitto::capture {

// X11 and Wayland: Qt requests UTF8_STRING / text/plain;charset=utf-8 itself, and the
// Wayland backends read the selection on their own.
NativeClipboardRead readNativeClipboard()
{
    return {NativeClipboardRead::Status::Unsupported, {}, {}};
}

} // namespace clipitto::capture
