#include "runtime_offsets.h"
#include "offsets_generated.hpp"

#include <windows.h>
#include <cstdlib>
#include <vector>

namespace
{
    struct Binding
    {
        const char* key;
        uintptr_t* destination;
        bool moduleRelative;
    };

    bool ParseUnsigned(const std::string& json, const char* key, uintptr_t& value)
    {
        const std::string needle = std::string("\"") + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return false;
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
        bool quoted = pos < json.size() && json[pos] == '"';
        if (quoted) ++pos;
        const char* begin = json.c_str() + pos;
        char* end = NULL;
        const unsigned long long parsed = strtoull(begin, &end, 0);
        if (end == begin || parsed == 0 || parsed > static_cast<unsigned long long>(UINTPTR_MAX))
            return false;
        if (quoted && *end != '"') return false;
        value = static_cast<uintptr_t>(parsed);
        return true;
    }
}

bool OakApplyRuntimeOffsets(const std::string& payload, bool apply)
{
    Binding bindings[] = {
        { "modbase.World", &oak_offsets::modbase::World, true },
        { "modbase.FOV_Context", &oak_offsets::modbase::FOV_Context, true },
        { "modbase.FovBase", &oak_offsets::modbase::FovBase, true },
        { "modbase.Network", &oak_offsets::modbase::Network, true },
        { "modbase.NetworkManager", &oak_offsets::modbase::NetworkManager, true },
        { "modbase.Tick", &oak_offsets::modbase::Tick, true },
        { "world.Camera", &oak_offsets::world::Camera, false },
        { "world.LocalPlayer", &oak_offsets::world::LocalPlayer, false },
        { "world.PlayerOn", &oak_offsets::world::PlayerOn, false },
        { "world.NearEntList", &oak_offsets::world::NearEntList, false },
        { "world.FarEntList", &oak_offsets::world::FarEntList, false },
        { "world.FarTableSize", &oak_offsets::world::FarTableSize, false },
        { "world.SlowEntList", &oak_offsets::world::SlowEntList, false },
        { "world.SlowTableSize", &oak_offsets::world::SlowTableSize, false },
        { "world.BulletList", &oak_offsets::world::BulletList, false },
        { "world.BulletCount", &oak_offsets::world::BulletCount, false },
        { "world.ItemList", &oak_offsets::world::ItemList, false },
        { "world.ItemListSize", &oak_offsets::world::ItemListSize, false },
        { "world.NoGrass", &oak_offsets::world::NoGrass, false },
        { "world.GrassOffline", &oak_offsets::world::GrassOffline, false },
        { "world.EyeAccom", &oak_offsets::world::EyeAccom, false },
        { "world.Hour", &oak_offsets::world::Hour, false },
        { "world.Day", &oak_offsets::world::Day, false },
        { "world.DayTime", &oak_offsets::world::DayTime, false },
        { "world.WeatherController", &oak_offsets::world::WeatherController, false },
        { "world.SlowEntValidCount", &oak_offsets::world::SlowEntValidCount, false },
        { "camera.InvertedViewRight", &oak_offsets::camera::InvertedViewRight, false },
        { "camera.InvertedViewUp", &oak_offsets::camera::InvertedViewUp, false },
        { "camera.InvertedViewForward", &oak_offsets::camera::InvertedViewForward, false },
        { "camera.InvertedViewTranslation", &oak_offsets::camera::InvertedViewTranslation, false },
        { "camera.ViewPortSize", &oak_offsets::camera::ViewPortSize, false },
        { "camera.GetProjectionD1", &oak_offsets::camera::GetProjectionD1, false },
        { "camera.GetProjectionD2", &oak_offsets::camera::GetProjectionD2, false },
        { "entity.Type", &oak_offsets::entity::Type, false },
        { "entity.VisualState", &oak_offsets::entity::VisualState, false },
        { "entity.FutureVisualState", &oak_offsets::entity::FutureVisualState, false },
        { "entity.IsDead", &oak_offsets::entity::IsDead, false },
        { "entity.NetworkId", &oak_offsets::entity::NetworkId, false },
        { "entity.NetworkIdPlayer", &oak_offsets::entity::NetworkIdPlayer, false },
        { "entity.Stamina", &oak_offsets::entity::Stamina, false },
        { "player.Skeleton", &oak_offsets::player::Skeleton, false },
        { "player.Inventory", &oak_offsets::player::Inventory, false },
        { "player.InputController", &oak_offsets::player::InputController, false },
        { "player.StatsContainer", &oak_offsets::player::StatsContainer, false },
        { "player.RecordValue", &oak_offsets::player::RecordValue, false },
        { "player.DamageManager", &oak_offsets::player::DamageManager, false },
        { "network.ScoreboardSize", &oak_offsets::network::ScoreboardSize, false },
        { "network.ThirdPersonFlag", &oak_offsets::network::ThirdPersonFlag, false },
        { "network.ManagerNetworkClient", &oak_offsets::network::ManagerNetworkClient, false },
        { "network.PlayerName", &oak_offsets::network::PlayerName, false },
        { "network.ScoreboardPtr", &oak_offsets::network::ScoreboardPtr, false },
        { "network.IdentityCount", &oak_offsets::network::IdentityCount, false },
        { "scoreboard_identity.NetworkId", &oak_offsets::scoreboard_identity::NetworkId, false },
        { "scoreboard_identity.SteamId", &oak_offsets::scoreboard_identity::SteamId, false },
        { "scoreboard_identity.Name", &oak_offsets::scoreboard_identity::Name, false },
        { "player_identity.Name", &oak_offsets::player_identity::Name, false },
        { "entity_owner.Owner", &oak_offsets::entity_owner::Owner, false },
        { "infected.Skeleton", &oak_offsets::infected::Skeleton, false },
        { "entitytype.TypeName", &oak_offsets::entitytype::TypeName, false },
        { "entitytype.ConfigName", &oak_offsets::entitytype::ConfigName, false },
        { "entitytype.ModelName", &oak_offsets::entitytype::ModelName, false },
        { "anim.MatrixArray", &oak_offsets::anim::MatrixArray, false },
        { "anim.MatrixB", &oak_offsets::anim::MatrixB, false },
        { "anim.AnimComponent", &oak_offsets::anim::AnimComponent, false },
        { "skeleton.AnimClass1", &oak_offsets::skeleton::AnimClass1, false },
        { "skeleton.AnimClass2", &oak_offsets::skeleton::AnimClass2, false },
        { "skeleton.AnimComponent", &oak_offsets::skeleton::AnimComponent, false },
    };

    std::vector<uintptr_t> parsed(ARRAYSIZE(bindings));
    for (size_t i = 0; i < ARRAYSIZE(bindings); ++i)
    {
        if (!ParseUnsigned(payload, bindings[i].key, parsed[i]))
            return false;
        // Module RVAs are larger but should still remain inside a conservative
        // 512 MiB image range. Structure offsets are capped at 1 MiB.
        const uintptr_t maximum = bindings[i].moduleRelative ? 0x20000000u : 0x100000u;
        if (parsed[i] >= maximum) return false;
    }
    if (apply)
    {
        for (size_t i = 0; i < ARRAYSIZE(bindings); ++i)
            *bindings[i].destination = parsed[i];
    }
    return true;
}
