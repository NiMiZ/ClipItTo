#pragma once

#include <QString>

namespace clipitto {

// Puts `text` on the macOS pasteboard marked as concealed (org.nspasteboard.ConcealedType,
// the type password managers set), so clipboard managers that honour nspasteboard.org skip
// it. Qt can't write that type: it renames types it doesn't know.
// Returns the pasteboard's change count right after the write: the token for clearPasteboardIfUnchanged().
long setConcealedText(const QString& text);

// Empties the pasteboard, but only if its change count is still `changeCount` (nothing was copied since, by
// anyone). Returns true when it was emptied.
bool clearPasteboardIfUnchanged(long changeCount);

// Empties the pasteboard whatever it holds (the panic key).
void clearPasteboard();

} // namespace clipitto
