#include "PackagedApp.h"

#if defined(_WIN32)
#include <windows.h>

#include <appmodel.h>
#endif

namespace clipitto::packaging {

namespace {
std::optional<bool> g_override;

bool probe()
{
#if defined(_WIN32)
    UINT32 length = 0;
    return packagedFromResult(GetCurrentPackageFullName(&length, nullptr));
#else
    return false;
#endif
}
} // namespace

bool packagedFromResult(long result)
{
    return result == 0 /* ERROR_SUCCESS */ || result == 122 /* ERROR_INSUFFICIENT_BUFFER */;
}

bool isPackaged()
{
    if (g_override) {
        return *g_override;
    }
    static const bool answer = probe();
    return answer;
}

void setPackagedOverrideForTest(std::optional<bool> value)
{
    g_override = value;
}

} // namespace clipitto::packaging
