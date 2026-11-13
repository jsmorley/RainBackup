#pragma once
#include "Model.h"

namespace rb {
class RainmeterSession {
public:
    void Stop(const Progress& progress);
    std::wstring Restart();
private:
    std::vector<fs::path> stopped_;
};
}
