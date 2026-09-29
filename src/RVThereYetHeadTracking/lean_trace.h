#pragma once

#include "cameraunlock/unreal/ue_math.h"

namespace RVThereYetHeadTracking {
struct Config;
namespace lean_trace {
void Configure(const Config& settings);
// Game-state thread only: the reflection lookups are full UObject scans.
void ResolveReflection();
void Reset();
cameraunlock::unreal::FVector Clamp(void* viewOwner, void* view,
                                  const cameraunlock::unreal::FVector& wanted);
}
}
