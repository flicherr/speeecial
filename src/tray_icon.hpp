#pragma once

#include <windows.h>

namespace snow
{
// The returned icon is owned by the caller and must be destroyed with
// DestroyIcon. No application resources are changed by adding the badge.
HICON load_tray_icon(HINSTANCE instance, int width, int height,
                     bool update_available, bool dark_background);
}
