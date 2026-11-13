#pragma once

#include <windows.h>

namespace rb {
inline constexpr UINT SetupCloseMessage = WM_APP + 20;
inline constexpr LRESULT SetupCloseAccepted = 1;
inline constexpr LRESULT SetupCloseBusy = 2;
int CloseRainBackupForSetup();
}
