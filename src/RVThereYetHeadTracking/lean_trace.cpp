#include "lean_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <windows.h>

#include "config.h"
#include "logging.h"
#include "cameraunlock/camera/lean_clamp.h"
#include "cameraunlock/memory/safe_memory.h"
#include "cameraunlock/time/qpc_clock.h"
#include "cameraunlock/unreal/ue_runtime.h"

namespace RVThereYetHeadTracking::lean_trace {
namespace {
namespace ue = cameraunlock::unreal;
using cameraunlock::camera::LeanObstruction;
using cameraunlock::math::Vec3;
using cameraunlock::memory::SafeRead;
using ProcessEvent = void(__fastcall*)(void*, void*, void*);

struct Field { std::size_t offset = 0; std::size_t size = 0; std::uint8_t mask = 0; };
struct ActorArray { const std::uintptr_t* data; std::int32_t count, capacity; };
struct Layout {
    std::array<Field, 11> params{};
    Field location, blocking, penetrating, depth, pawn, nearPlane, viewNearPlane;
    std::uintptr_t function = 0, library = 0, engine = 0;
    ProcessEvent dispatch = nullptr;
};
// Written once by ResolveReflection on the game-state thread, then published
// to the game thread by the release store to g_ready.
Layout g_layout;
std::atomic<bool> g_ready{false}, g_failed{false};
bool g_enabled = true;
float g_margin = 15.0f, g_release = 0.9f;
int g_channel = 0;

bool ReadField(std::uintptr_t owner, const char* name, const char* type, std::size_t width, Field& out)
{
    std::uint32_t total = 0;
    std::uintptr_t field = 0;
    if (!ue::SafeReadU32(owner + 0x58, total) || !ue::SafeReadPtr(owner + 0x50, field)) return false;
    for (unsigned count = 0; field && count < 1024; ++count) {
        std::uint32_t id = 0;
        if (!ue::SafeReadU32(field + 0x20, id)) return false;
        if (ue::ResolveFName(id) == name) {
            std::uintptr_t fieldClass = 0;
            std::uint32_t typeId = 0, offset = 0, size = 0, dim = 0;
            if (!ue::SafeReadPtr(field + 8, fieldClass) || !ue::SafeReadU32(fieldClass, typeId) ||
                ue::ResolveFName(typeId) != type || !ue::SafeReadU32(field + 0x44, offset) ||
                !ue::SafeReadU32(field + 0x34, size) || !ue::SafeReadU32(field + 0x30, dim) ||
                dim != 1 || size != width || offset > total || size > total - offset) return false;
            out = {offset, size, 0};
            if (std::strcmp(type, "BoolProperty") == 0) {
                // UE5.6 stores the bool descriptor at FBoolProperty + 0x70;
                // the UE4 adapter uses +0x78. Check the descriptor before use.
                std::array<std::uint8_t, 4> bits{};
                if (!SafeRead(field + 0x70, bits) || bits[0] != 1 || bits[1] != 0 || bits[2] == 0 ||
                    (bits[3] != 0xff && bits[3] != bits[2])) return false;
                out.mask = bits[2];
            }
            return true;
        }
        if (!ue::SafeReadPtr(field + 0x18, field)) return false;
    }
    return false;
}

void ResolveLayout()
{
    auto& l = g_layout;
    l.function = ue::FindLiveObject("Function", "SphereTraceSingle", "KismetSystemLibrary");
    l.library = ue::FindLiveObject("KismetSystemLibrary", "Default__KismetSystemLibrary", nullptr);
    l.engine = ue::FindLiveObject("GameEngine", "GameEngine", nullptr);
    const auto hit = ue::FindLiveObject("ScriptStruct", "HitResult", "/Script/Engine");
    const auto controller = ue::FindLiveObject("Class", "Controller", "/Script/Engine");
    const auto engine = ue::FindLiveObject("Class", "Engine", "/Script/Engine");
    const auto view = ue::FindLiveObject("ScriptStruct", "MinimalViewInfo", "/Script/Engine");
    if (!l.function || !l.library || !l.engine || !hit || !controller || !engine || !view) return;
    std::uint32_t frameSize = 0, hitSize = 0;
    bool valid = ue::SafeReadU32(l.function + 0x58, frameSize) && frameSize > 0 && frameSize <= 1024 &&
        ue::SafeReadU32(hit + 0x58, hitSize) && hitSize > 0 && hitSize <= frameSize;
    struct Parameter { const char* name; const char* type; std::size_t size; };
    const Parameter parameters[] = {
        {"WorldContextObject", "ObjectProperty", 8}, {"Start", "StructProperty", sizeof(ue::FVector)},
        {"End", "StructProperty", sizeof(ue::FVector)}, {"Radius", "FloatProperty", 4},
        {"TraceChannel", "ByteProperty", 1}, {"bTraceComplex", "BoolProperty", 1},
        {"ActorsToIgnore", "ArrayProperty", sizeof(ActorArray)}, {"DrawDebugType", "ByteProperty", 1},
        {"OutHit", "StructProperty", hitSize}, {"bIgnoreSelf", "BoolProperty", 1},
        {"ReturnValue", "BoolProperty", 1},
    };
    for (std::size_t i = 0; i < l.params.size(); ++i) {
        if (!ReadField(l.function, parameters[i].name, parameters[i].type, parameters[i].size, l.params[i])) {
            Log::Line("lean-trace: incompatible SphereTraceSingle.%s", parameters[i].name);
            valid = false;
        }
    }
    valid = ReadField(hit, "Location", "StructProperty", sizeof(ue::FVector), l.location) && valid;
    valid = ReadField(hit, "bBlockingHit", "BoolProperty", 1, l.blocking) && valid;
    valid = ReadField(hit, "bStartPenetrating", "BoolProperty", 1, l.penetrating) && valid;
    valid = ReadField(hit, "PenetrationDepth", "FloatProperty", 4, l.depth) && valid;
    valid = ReadField(controller, "Pawn", "ObjectProperty", 8, l.pawn) && valid;
    valid = ReadField(engine, "NearClipPlane", "FloatProperty", 4, l.nearPlane) && valid;
    valid = ReadField(view, "PerspectiveNearClipPlane", "FloatProperty", 4, l.viewNearPlane) && valid;
    std::uintptr_t table = 0, dispatch = 0;
    valid = ue::SafeReadPtr(l.library, table) && ue::SafeReadPtr(table + 76 * 8, dispatch) &&
        dispatch >= ue::ModuleBase() && dispatch < ue::ModuleEnd() && valid;
    if (!valid) {
        g_failed.store(true);
        Log::Line("lean-trace: incompatible reflection layout; positional lean withheld");
        return;
    }
    l.dispatch = reinterpret_cast<ProcessEvent>(dispatch);
    g_ready.store(true, std::memory_order_release);
    Log::Line("lean-trace: SphereTraceSingle resolved; frame=%u hit=%u pawn=+0x%zx near=+0x%zx viewNear=+0x%zx margin=%.1fcm channel=%d",
        frameSize, hitSize, l.pawn.offset, l.nearPlane.offset, l.viewNearPlane.offset, g_margin, g_channel);
}

struct QueryContext {
    ue::FVector eye;
    std::uintptr_t pawn;
    float radius, nearPlane;
};

LeanObstruction Query(void* context, const Vec3&, const Vec3& direction, float distance)
{
    auto& q = *static_cast<QueryContext*>(context);
    const auto& l = g_layout;
    const ue::FVector end{q.eye.X + direction.x * distance, q.eye.Y + direction.y * distance,
        q.eye.Z + direction.z * distance};
    float radius = q.radius;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        alignas(16) std::array<std::uint8_t, 1024> frame{};
        const auto set = [&](std::size_t index, const auto& value) {
            std::memcpy(frame.data() + l.params[index].offset, &value, sizeof(value));
        };
        set(0, q.pawn);
        set(1, q.eye);
        set(2, end);
        set(3, radius);
        set(4, static_cast<std::uint8_t>(g_channel));
        set(9, l.params[9].mask);
        l.dispatch(reinterpret_cast<void*>(l.library), reinterpret_cast<void*>(l.function), frame.data());
        const auto* hit = frame.data() + l.params[8].offset;
        if (!(hit[l.blocking.offset] & l.blocking.mask)) return {true, false, 0};
        if (hit[l.penetrating.offset] & l.penetrating.mask) {
            float depth = 0;
            std::memcpy(&depth, hit + l.depth.offset, sizeof(depth));
            if (!std::isfinite(depth) || depth < 0) return {};
            // Shrink only spare clearance when the clean eye starts near a
            // surface. Never shrink the sweep inside the live near plane.
            const float smaller = radius - depth - 0.05f;
            if (smaller <= q.nearPlane + 0.5f) return {true, true, 0};
            radius = smaller;
            continue;
        }
        ue::FVector location{};
        std::memcpy(&location, hit + l.location.offset, sizeof(location));
        const double room = (location.X - q.eye.X) * direction.x + (location.Y - q.eye.Y) * direction.y +
            (location.Z - q.eye.Z) * direction.z - 0.05;
        if (!std::isfinite(room)) return {};
        return {true, true, static_cast<float>(std::max(0.0, room))};
    }
    return {};
}

struct CameraState {
    cameraunlock::camera::LeanClamp clamp;
    ue::FVector eye{}, wanted{}, allowed{};
    std::uintptr_t pawn = 0;
    std::uint64_t lastUs = 0, lastLog = 0;
    bool contact = false, failed = false;
};
std::unordered_map<void*, CameraState> g_cameras;
}

void Configure(const Config& config)
{
    g_enabled = config.collision_enabled;
    g_margin = config.collision_margin;
    g_channel = config.collision_channel;
    g_release = config.collision_release_smoothing;
    if (g_channel < 0 || g_channel > 31 || !std::isfinite(g_margin) || g_margin < 0) {
        g_failed.store(true);
        Log::Line("lean-trace: invalid margin/channel; positional lean withheld while collision is enabled");
    }
}

void ResolveReflection()
{
    if (!g_enabled || g_ready.load(std::memory_order_acquire) || g_failed.load()) return;
    ResolveLayout();
}

void Reset() { g_cameras.clear(); }

ue::FVector Clamp(void* owner, void* view, const ue::FVector& wanted)
{
    if (!g_enabled) return wanted;
    if (!g_ready.load(std::memory_order_acquire) || g_failed.load()) {
        static std::uint64_t lastLog = 0;
        const auto now = GetTickCount64();
        if (now - lastLog >= 5000) {
            lastLog = now;
            Log::Line("lean-trace: query unavailable; positional lean withheld");
        }
        return {};
    }
    const auto& l = g_layout;
    std::uintptr_t controller = 0, pawn = 0;
    float nearPlane = 0, viewNear = 0;
    if (!ue::SafeReadPtr(reinterpret_cast<std::uintptr_t>(owner) + 0x30, controller) || !controller ||
        !ue::SafeReadPtr(controller + l.pawn.offset, pawn) || !pawn ||
        !ue::SafeReadFloat(l.engine + l.nearPlane.offset, nearPlane) ||
        !ue::SafeReadFloat(reinterpret_cast<std::uintptr_t>(view) + l.viewNearPlane.offset, viewNear) ||
        !std::isfinite(nearPlane) || !std::isfinite(viewNear)) {
        static std::uint64_t lastLog = 0;
        const auto now = GetTickCount64();
        if (now - lastLog >= 5000) {
            lastLog = now;
            Log::Line("lean-trace: pawn or near-plane context unavailable; positional lean withheld");
        }
        Reset();
        return {};
    }
    if (viewNear > 0) nearPlane = viewNear;
    if (nearPlane <= 0) {
        if (!g_failed.exchange(true))
            Log::Line("lean-trace: invalid live near plane %.3f; positional lean withheld", nearPlane);
        Reset();
        return {};
    }
    const auto eye = *static_cast<const ue::FVector*>(view);
    QueryContext context{eye, pawn, std::max(g_margin, nearPlane + 1.0f), nearPlane};
    auto& state = g_cameras[owner];
    const auto now = cameraunlock::time::QpcNowMicros();
    // The builder runs several times a frame for the same view. A repeat with
    // the same eye and lean inside the frame gets the answer the first call's
    // sweep gave, instead of sweeping the world again.
    const auto same = [](const ue::FVector& a, const ue::FVector& b) {
        return a.X == b.X && a.Y == b.Y && a.Z == b.Z;
    };
    if (state.lastUs != 0 && now - state.lastUs < 500 && state.pawn == pawn &&
        same(state.eye, eye) && same(state.wanted, wanted))
        return state.allowed;
    const double dx = eye.X - state.eye.X, dy = eye.Y - state.eye.Y, dz = eye.Z - state.eye.Z;
    if (state.pawn != pawn || now - state.lastUs > 250000 || dx * dx + dy * dy + dz * dz > 10000) state.clamp.Reset();
    const float dt = std::min(0.1f, static_cast<float>(now - state.lastUs) * 1e-6f);
    state.clamp.SetSettings({0.0f, g_release});
    // Keep large UE5 world coordinates in double precision. Core needs only
    // the relative offset; the query carries the clean world-space eye.
    const Vec3 desired{static_cast<float>(wanted.X), static_cast<float>(wanted.Y), static_cast<float>(wanted.Z)};
    auto allowed = state.clamp.Apply({}, desired, dt, Query, &context);
    if (state.clamp.LastQueryFailed()) allowed = {};
    const bool contact = state.clamp.InContact(), failed = state.clamp.LastQueryFailed();
    if (contact != state.contact || failed != state.failed || now - state.lastLog >= 2000000) {
        Log::Line("lean-clamp: wanted=%.2f allowed=%.2fcm contact=%d failed=%d radius=%.2f near=%.2f eye=(%.1f,%.1f,%.1f) offset=(%.2f,%.2f,%.2f)",
            desired.Magnitude(), allowed.Magnitude(), contact, failed, context.radius, nearPlane,
            eye.X, eye.Y, eye.Z, wanted.X, wanted.Y, wanted.Z);
        state.lastLog = now;
    }
    state.eye = eye;
    state.wanted = wanted;
    state.allowed = {allowed.x, allowed.y, allowed.z};
    state.pawn = pawn;
    state.lastUs = now;
    state.contact = contact;
    state.failed = failed;
    return state.allowed;
}
}
