#pragma once

#include <cstdint>

#include "builds/build_registry.h"
#include "cameraunlock/unreal/ue_runtime.h"

namespace RVThereYetHeadTracking
{
    // A cached UObject pointer plus the class it had when it was found.
    struct LiveObject { std::uintptr_t obj = 0; std::uintptr_t cls = 0; };

    // Is the object still in the global UObject array at the slot its own
    // InternalIndex names? A freed UObject leaves its memory readable and its
    // ClassPrivate intact for as long as the allocator holds the block, so the
    // class-pointer test on its own reports dead objects as live.
    inline bool RegisteredInObjectArray(std::uintptr_t obj)
    {
        namespace ue = ::cameraunlock::unreal;
        const auto& g = Offsets().UObjectGlobals;
        if (g.kChunkNumElems == 0 || g.kFUObjectItemSize == 0) return false;
        // UObjectBase packs InternalIndex immediately before ClassPrivate.
        std::uint32_t index = 0;
        if (!ue::SafeReadU32(obj + g.kClassPrivate - 4, index)) return false;

        const std::uintptr_t objArr = ue::ModuleBase() + g.kObjObjects;
        std::uintptr_t chunks = 0;
        std::uint32_t num = 0;
        if (!ue::SafeReadPtr(objArr, chunks) || !chunks) return false;
        if (!ue::SafeReadU32(objArr + g.kObjObjects_Num, num) || index >= num) return false;

        std::uintptr_t chunk = 0;
        if (!ue::SafeReadPtr(chunks + (static_cast<std::uintptr_t>(index / g.kChunkNumElems) * 8),
                             chunk) || !chunk)
            return false;
        std::uintptr_t registered = 0;
        return ue::SafeReadPtr(chunk + static_cast<std::uintptr_t>(index % g.kChunkNumElems)
                                   * g.kFUObjectItemSize, registered)
            && registered == obj;
    }

    inline LiveObject CaptureLiveObject(std::uintptr_t obj)
    {
        std::uintptr_t cls = 0;
        ::cameraunlock::unreal::SafeReadPtr(obj + Offsets().UObjectGlobals.kClassPrivate, cls);
        return { obj, cls };
    }

    // A cached object goes stale when it is freed or its slot is reused.
    inline bool IsLive(const LiveObject& o)
    {
        if (!o.obj || !o.cls) return false;
        std::uintptr_t cls = 0;
        if (!::cameraunlock::unreal::SafeReadPtr(o.obj + Offsets().UObjectGlobals.kClassPrivate, cls)
            || cls != o.cls)
            return false;
        return RegisteredInObjectArray(o.obj);
    }
}
