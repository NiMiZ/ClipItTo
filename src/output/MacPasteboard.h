#pragma once

#include <QString>

namespace clipitto {

// Puts `text` on the macOS pasteboard marked as concealed (org.nspasteboard.ConcealedType,
// the type password managers set), so clipboard managers that honour nspasteboard.org skip
// it. Qt can't write that type: it renames types it doesn't know.
void setConcealedText(const QString& text);

} // namespace clipitto
