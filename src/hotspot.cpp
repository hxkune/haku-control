// SPDX-License-Identifier: GPL-3.0-only
// haku-control-hotspot: turns on the Windows Mobile Hotspot, sharing the wired / USB internet connection.
// Started by haku control only when the hotspot is off; exits right away.
// Exit codes: 0 already on, 1 started, 2 nothing to share, 3 refused, 4 error.
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

#pragma comment(lib, "windowsapp")

using namespace winrt;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::NetworkOperators;

static ConnectionProfile pick_profile() {
    // prefer a non-Wi-Fi connection with internet (e.g. a phone tethered over USB): the Wi-Fi radio hosts the hotspot
    for (auto const &p : NetworkInformation::GetConnectionProfiles())
        if (!p.IsWlanConnectionProfile() && p.GetNetworkConnectivityLevel() == NetworkConnectivityLevel::InternetAccess)
            return p;
    return NetworkInformation::GetInternetConnectionProfile();
}

int main() {
    try {
        init_apartment();
        auto prof = pick_profile();
        if (!prof) return 2;
        auto m = NetworkOperatorTetheringManager::CreateFromConnectionProfile(prof);
        if (m.TetheringOperationalState() == TetheringOperationalState::On) return 0;
        auto r = m.StartTetheringAsync().get();
        return r.Status() == TetheringOperationStatus::Success ? 1 : 3;
    } catch (...) {
        return 4;
    }
}
