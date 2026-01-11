#include "DebugDrawingPacketHandler.h"
#include "BedrockServerClientInterface.h"
#include "bsci/particle/ParticleSpawner.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/packet/DebugDrawerPacket.h"
#include "mc/network/packet/DebugDrawerPacketPayload.h"
#include "mc/network/packet/SetLocalPlayerAsInitializedPacket.h"
#include "mc/network/packet/ShapeDataPayload.h"
#include "mc/network/packet/SphereDataPayload.h"
#include "mc/network/packet/TextDataPayload.h"
#include <parallel_hashmap/phmap.h>
#include <string>
#include <utility>


TextDataPayload::TextDataPayload() = default;
TextDataPayload::TextDataPayload(TextDataPayload const& cp) { mText = cp.mText; };
ShapeDataPayload::ShapeDataPayload() { mNetworkId = 0; };
DebugDrawerPacketPayload::DebugDrawerPacketPayload()                                = default;
DebugDrawerPacketPayload::DebugDrawerPacketPayload(DebugDrawerPacketPayload const&) = default;

template <class K, class V, size_t N = 4, class M = std::shared_mutex>
using ph_flat_hash_map = phmap::parallel_flat_hash_map<
    K,
    V,
    phmap::priv::hash_default_hash<K>,
    phmap::priv::hash_default_eq<K>,
    phmap::priv::Allocator<phmap::priv::Pair<const K, V>>,
    N,
    M>;


namespace bsci {
// std::unique_ptr<GeometryGroup> GeometryGroup::createDefault() {
//     return std::make_unique<DebugDrawingPacketHandler>();
// }

static std::atomic<uint64_t> nextId_{UINT64_MAX};

class DebugDrawingPacketHandler::Impl {
public:
    struct Hook;
    size_t                           id{};
    std::unique_ptr<ParticleSpawner> particleSpawner =
        std::make_unique<ParticleSpawner>(); // 向前兼容
    ph_flat_hash_map<GeoId, std::pair<std::unique_ptr<DebugDrawerPacket>, bool>, 6>
        geoPackets; // bool = true 表示使用ParticleSpawner

public:
    void sendPacketImmediately(DebugDrawerPacket& pkt) {
        if (pkt.mShapes->empty()) return;
        ll::thread::ServerThreadExecutor::getDefault().execute([pkt] { pkt.sendToClients(); });
    }
};

static std::recursive_mutex                    listMutex;
static std::vector<DebugDrawingPacketHandler*> list;
static std::atomic_bool                        hasInstance{false};
static size_t                                  listtick;

LL_TYPE_INSTANCE_HOOK(
    DebugDrawingPacketHandler::Impl::Hook,
    HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::$handle,
    void,
    NetworkIdentifier const&                 identifier,
    SetLocalPlayerAsInitializedPacket const& packet
) {
    origin(identifier, packet);
    if (hasInstance) {
        std::lock_guard l{listMutex};
        listtick++;
        for (auto s : list) {
            for (auto& [id, pkt] : s->impl->geoPackets) {
                if (pkt.first) pkt.first->sendToClient(identifier, packet.mSenderSubId);
            }
        }
    }
}

DebugDrawingPacketHandler::DebugDrawingPacketHandler() : impl(std::make_unique<Impl>()) {
    static ll::memory::HookRegistrar<DebugDrawingPacketHandler::Impl::Hook> reg;
    std::lock_guard                                                         l{listMutex};
    hasInstance = true;
    impl->id    = list.size();
    list.push_back(this);
}

DebugDrawingPacketHandler::~DebugDrawingPacketHandler() {
    std::lock_guard l{listMutex};
    list.back()->impl->id = impl->id;
    std::swap(list[impl->id], list.back());
    list.pop_back();
    hasInstance = !list.empty();
}

GeometryGroup::GeoId DebugDrawingPacketHandler::point(
    DimensionType        dim,
    Vec3 const&          pos,
    mce::Color const&    color,
    std::optional<float> radius
) {
    auto geoId = this->impl->particleSpawner->point(dim, pos, color, radius);
    this->impl->geoPackets.emplace(geoId, std::make_pair(nullptr, true));
    return geoId;
}

GeometryGroup::GeoId DebugDrawingPacketHandler::line(
    DimensionType        dim,
    Vec3 const&          begin,
    Vec3 const&          end,
    mce::Color const&    color,
    std::optional<float> thickness
) {
    if (begin == end) return {0};
    if (thickness.has_value()) {
        auto geoId = this->impl->particleSpawner->line(dim, begin, end, color, thickness);
        this->impl->geoPackets.emplace(geoId, std::make_pair(nullptr, true));
        return geoId;
    }

    Vec3 offset      = end - begin;
    int  segmentNum  = ((int)offset.length()) / 150;
    offset          /= segmentNum + 1;
    BedrockServerClientInterface::getInstance().getSelf().getLogger().warn(
        "切分数：" + std::to_string(segmentNum + 1)
    );
    Vec3 currentPos = begin;

    auto packet = std::make_unique<DebugDrawerPacket>();
    packet->setSerializationMode(SerializationMode::CerealOnly);
    for (int i = 0; i < segmentNum; i++) {
        ShapeDataPayload shape;
        shape.mNetworkId         = nextId_.fetch_sub(1);
        shape.mShapeType         = ScriptModuleDebugUtilities::ScriptDebugShapeType::Line;
        shape.mLocation          = currentPos;
        currentPos              += offset;
        shape.mColor             = color;
        shape.mDimensionId       = dim;
        shape.mExtraDataPayload  = LineDataPayload{.mEndLocation = currentPos};
        packet->mShapes->emplace_back(std::move(shape));
    }
    ShapeDataPayload shape;
    shape.mNetworkId        = nextId_.fetch_sub(1);
    shape.mShapeType        = ScriptModuleDebugUtilities::ScriptDebugShapeType::Line;
    shape.mLocation         = currentPos;
    shape.mColor            = color;
    shape.mDimensionId      = dim;
    shape.mExtraDataPayload = LineDataPayload{.mEndLocation = end}; // 避免浮点误差
    packet->mShapes->emplace_back(std::move(shape));

    packet->sendToClients();
    auto id = GeometryGroup::getNextGeoId();
    this->impl->geoPackets.emplace(id, std::move(packet));
    return id;
}

GeometryGroup::GeoId DebugDrawingPacketHandler::box(
    DimensionType        dim,
    AABB const&          box,
    mce::Color const&    color,
    std::optional<float> thickness
) {
    if (thickness.has_value()) {
        auto geoId = this->impl->particleSpawner->box(dim, box, color, thickness);
        this->impl->geoPackets.emplace(geoId, nullptr);
        return geoId;
    }
    if ((box.max - box.min).lengthSqr() >= 22500) return GeometryGroup::box(dim, box, color);

    auto packet = std::make_unique<DebugDrawerPacket>();
    packet->setSerializationMode(SerializationMode::CerealOnly);
    ShapeDataPayload shape;
    shape.mNetworkId        = nextId_.fetch_sub(1);
    shape.mShapeType        = ScriptModuleDebugUtilities::ScriptDebugShapeType::Box;
    shape.mLocation         = box.min;
    shape.mColor            = color;
    shape.mDimensionId      = dim;
    shape.mExtraDataPayload = BoxDataPayload{.mBoxBound = box.max - box.min};
    packet->mShapes->emplace_back(std::move(shape));
    packet->sendToClients();
    auto id = GeometryGroup::getNextGeoId();
    this->impl->geoPackets.emplace(id, std::move(packet));
    return id;
}

GeometryGroup::GeoId DebugDrawingPacketHandler::circle2(
    DimensionType     dim,
    Vec3 const&       center,
    Vec3 const&       normal,
    float             radius,
    mce::Color const& color
) {
    if (radius > 150) return this->circle(dim, center, normal, radius, color);

    auto packet = std::make_unique<DebugDrawerPacket>();
    packet->setSerializationMode(SerializationMode::CerealOnly);
    ShapeDataPayload shape;
    shape.mNetworkId   = nextId_.fetch_sub(1);
    shape.mShapeType   = ScriptModuleDebugUtilities::ScriptDebugShapeType::Circle;
    shape.mRotation    = normal;
    shape.mLocation    = center;
    shape.mScale       = radius;
    shape.mColor       = color;
    shape.mDimensionId = dim;
    packet->mShapes->emplace_back(std::move(shape));
    packet->sendToClients();
    auto id = GeometryGroup::getNextGeoId();
    this->impl->geoPackets.emplace(id, std::move(packet));
    return id;
}

GeometryGroup::GeoId DebugDrawingPacketHandler::sphere2(
    DimensionType        dim,
    Vec3 const&          center,
    float                radius,
    mce::Color const&    color,
    std::optional<uchar> mNumSegments
) {
    if (radius > 150) return this->sphere(dim, center, radius, color);

    auto packet = std::make_unique<DebugDrawerPacket>();
    packet->setSerializationMode(SerializationMode::CerealOnly);
    ShapeDataPayload shape;
    shape.mNetworkId   = nextId_.fetch_sub(1);
    shape.mShapeType   = ScriptModuleDebugUtilities::ScriptDebugShapeType::Sphere;
    shape.mLocation    = center;
    shape.mScale       = radius;
    shape.mColor       = color;
    shape.mDimensionId = dim;
    if (mNumSegments.has_value()) {
        shape.mExtraDataPayload = SphereDataPayload{.mNumSegments = mNumSegments.value()};
    }
    packet->mShapes->emplace_back(std::move(shape));
    packet->sendToClients();
    auto id = GeometryGroup::getNextGeoId();
    this->impl->geoPackets.emplace(id, std::move(packet));
    return id;
}

GeometryGroup::GeoId DebugDrawingPacketHandler::arrow(
    DimensionType        dim,
    Vec3 const&          begin,
    Vec3 const&          end,
    mce::Color const&    color,
    std::optional<float> mArrowHeadLength,
    std::optional<float> mArrowHeadRadius,
    std::optional<uchar> mNumSegments
) {
    if (begin == end) return {0};

    Vec3 offset      = end - begin;
    int  segmentNum  = ((int)offset.length()) / 150;
    offset          /= segmentNum + 1;
    BedrockServerClientInterface::getInstance().getSelf().getLogger().warn(
        "切分数：" + std::to_string(segmentNum + 1)
    );
    Vec3 currentPos = begin;

    auto packet = std::make_unique<DebugDrawerPacket>();
    packet->setSerializationMode(SerializationMode::CerealOnly);
    for (int i = 0; i < segmentNum; i++) {
        ShapeDataPayload shape;
        shape.mNetworkId         = nextId_.fetch_sub(1);
        shape.mShapeType         = ScriptModuleDebugUtilities::ScriptDebugShapeType::Line;
        shape.mLocation          = currentPos;
        currentPos              += offset;
        shape.mColor             = color;
        shape.mDimensionId       = dim;
        shape.mExtraDataPayload  = LineDataPayload{.mEndLocation = currentPos};
        packet->mShapes->emplace_back(std::move(shape));
    }
    ShapeDataPayload shape;
    shape.mNetworkId        = nextId_.fetch_sub(1);
    shape.mShapeType        = ScriptModuleDebugUtilities::ScriptDebugShapeType::Arrow;
    shape.mLocation         = currentPos;
    shape.mColor            = color;
    shape.mDimensionId      = dim;
    shape.mExtraDataPayload = ArrowDataPayload{
        .mEndLocation     = end,
        .mArrowHeadLength = mArrowHeadLength,
        .mArrowHeadRadius = mArrowHeadRadius,
        .mNumSegments     = mNumSegments
    };
    packet->mShapes->emplace_back(std::move(shape));

    packet->sendToClients();
    auto id = GeometryGroup::getNextGeoId();
    this->impl->geoPackets.emplace(id, std::move(packet));
    return id;
}
} // namespace bsci