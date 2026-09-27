#pragma once

#include "cameraunlock/unreal/ue_math.h"

namespace RVThereYetHeadTracking {
struct Config;
namespace lean_trace {
void Configure(const Config& settings);
void Reset();
cameraunlock::unreal::FVector Clamp(void* viewOwner, void* view,
                                  const cameraunlock::unreal::FVector& wanted);
}
}
