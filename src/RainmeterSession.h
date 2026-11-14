#pragma once
#include "Archive.h"

namespace rb {
class RainmeterSession {
public:
    void Stop(const Progress& progress);
    std::wstring Restart();
private:
    std::vector<fs::path> stopped_;
};
void RestoreWithRainmeterRestart(const OpenZip& archive, const RestorePlan& plan,
    const Progress& progress = {}, const Transfer& transfer = {});
}
