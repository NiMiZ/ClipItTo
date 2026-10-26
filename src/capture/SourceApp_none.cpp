#include "SourceApp.h"

namespace clipitto::capture {

// No way to learn the clipboard's owner here (e.g. wl-paste on KDE/wlroots): unknown.
QString clipboardSourceApp()
{
    return {};
}

} // namespace clipitto::capture
