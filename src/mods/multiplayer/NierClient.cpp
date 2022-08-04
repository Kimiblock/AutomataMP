#include <thread>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "schema/Packets_generated.h"
#include "Packets.hpp"
#include "mods/AutomataMPMod.hpp"
#include "NierClient.hpp"

using namespace std;

NierClient::NierClient(const std::string& host, const std::string& name, const std::string& password)
    : m_helloName{ name },
    m_password{ password }
{
    set_trace_handler([](const std::string& s) { spdlog::info("{}", s); });
    connect(enetpp::client_connect_params().set_channel_count(1).set_server_host_name_and_port(host.c_str(), 6969).set_timeout(chrono::seconds(1)));

    while (get_connection_state() == enetpp::CONNECT_CONNECTING) {
        think();
        this_thread::yield();
    }
}

NierClient::~NierClient() {
    disconnect();
}

void NierClient::think() {
    consume_events(
        [this]() { onConnect(); },
        [this]() { onDisconnect(); },
        [this](const enet_uint8* a, size_t b) { 
            onDataReceived(a, b); 
        }
    );

    if (m_helloSent && m_welcomeReceived && m_players.contains(m_guid)) {
        updateLocalPlayerData();
        sendPlayerData();

        // Synchronize the players.
        for (auto& it : m_players) {
            const auto& networkedPlayer = it.second;

            // Do not update the local player here.
            if (networkedPlayer == nullptr || networkedPlayer->getGuid() == m_guid) {
                continue;
            }

            auto npc = networkedPlayer->getEntity();

            if (npc == nullptr) {
                spdlog::error("NPC for player {} not found", networkedPlayer->getGuid());
                continue;
            }

            spdlog::info("Synchronizing player {}", networkedPlayer->getGuid());

            auto& data = networkedPlayer->getPlayerData();
            *npc->getRunSpeedType() = SPEED_PLAYER;
            *npc->getFlashlightEnabled() = data.flashlight();
            *npc->getSpeed() = data.speed();
            *npc->getFacing() = data.facing();
            *npc->getFacing2() = data.facing2();
            *npc->getWeaponIndex() = data.weapon_index();
            *npc->getPodIndex() = data.pod_index();
            npc->getCharacterController()->heldFlags = data.held_button_flags();
            //*npc->getPosition() = *(Vector3f*)&data.position();
        }
    }
}

void NierClient::on_draw_ui() {
}

void NierClient::onConnect() {
    if (auto ents = EntityList::get(); ents == nullptr || ents->getPossessedEntity() == nullptr) {
        AutomataMPMod::get()->signalDestroyClient();
        spdlog::error("Please spawn a player before connecting to the server.");
        return;
    }

    spdlog::set_default_logger(spdlog::basic_logger_mt("AutomataMPClient", "automatamp_clientlog.txt", true));
    spdlog::info("Connected");

    if (!m_welcomeReceived && !m_helloSent) {
        sendHello();
    }
}

void NierClient::onDisconnect() {
    spdlog::info("Disconnected");
}

void NierClient::onDataReceived(const enet_uint8* data, size_t size) {
    try {
        auto verif = flatbuffers::Verifier(data, size);
        const auto packet = flatbuffers::GetRoot<nier::Packet>(data);

        if (!packet->Verify(verif)) {
            spdlog::error("Invalid packet");
            return;
        }

        onPacketReceived(packet);
    } catch(const std::exception& e) {
        spdlog::error("Exception occurred during packet processing: {}", e.what());
    } catch(...) {
        spdlog::error("Unknown exception occurred during packet processing");
    }
}

void NierClient::onPacketReceived(const nier::Packet* packet) {
    if (!m_welcomeReceived && packet->id() != nier::PacketType_ID_WELCOME) {
        spdlog::error("Expected welcome packet, but got {} ({}), ignoring", packet->id(), nier::EnumNamePacketType(packet->id()));
        return;
    }

    const nier::PlayerPacket* playerPacket = nullptr;

    // Bounced player packets.
    if (packet->id() > nier::PacketType_ID_CLIENT_START && packet->id() < nier::PacketType_ID_CLIENT_END) {
        playerPacket = flatbuffers::GetRoot<nier::PlayerPacket>(packet->data()->data());
        flatbuffers::Verifier playerVerif(packet->data()->data(), packet->data()->size());

        if (!playerPacket->Verify(playerVerif)) {
            spdlog::error("Invalid player packet {} ({})", packet->id(), nier::EnumNamePacketType(packet->id()));
            return;
        }

        onPlayerPacketReceived(packet->id(), playerPacket);
        return;
    }

    // Standard packets.
    switch(packet->id()) {
        case nier::PacketType_ID_WELCOME: {
            if (handleWelcome(packet)) {
                m_welcomeReceived = true;
            }

            break;
        }

        case nier::PacketType_ID_CREATE_PLAYER: {
            if (!handleCreatePlayer(packet)) {
                spdlog::error("Failed to create player");
            }

            break;
        }

        case nier::PacketType_ID_DESTROY_PLAYER: {
            if (!handleDestroyPlayer(packet)) {
                spdlog::error("Failed to destroy player");
            }

            break;
        }

        default:
            spdlog::error("Unknown packet type {} ({})", packet->id(), nier::EnumNamePacketType(packet->id()));
            break;
    }

    /*if (data->id >= ID_SHARED_START && data->id < ID_SHARED_END) {
        AutomataMPMod::get()->sharedPacketProcess(data, size);
    }
    else if (data->id >= ID_SERVER_START && data->id < ID_SERVER_END) {
        AutomataMPMod::get()->serverPacketProcess(data, size);
    }*/
}

void NierClient::onPlayerPacketReceived(nier::PacketType packetType, const nier::PlayerPacket* packet) {
    spdlog::info("Player packet {} received from {}", nier::EnumNamePacketType(packetType), packet->guid());

    switch (packetType) {
        case nier::PacketType_ID_PLAYER_DATA: {
            if (!handlePlayerData(packet)) {
                spdlog::error("Failed to handle player data");
            }

            break;
        }
        case nier::PacketType_ID_ANIMATION_START: {
            if (!handleAnimationStart(packet)) {
                spdlog::error("Failed to handle animation start");
            }

            break;
        }
        case nier::PacketType_ID_BUTTONS: {
            if (!handleButtons(packet)) {
                spdlog::error("Failed to handle buttons");
            }

            break;
        }
        default:
            spdlog::error("Unknown player packet type {} ({})", packetType, nier::EnumNamePacketType(packetType));
            break;
    }
}

void NierClient::sendPacket(nier::PacketType id, const uint8_t* data, size_t size) {
    auto builder = flatbuffers::FlatBufferBuilder{};

    uint32_t dataoffs = 0;

    if (data != nullptr && size > 0) {
        builder.StartVector(size, 1); // byte vector
        for (int64_t i = (int64_t)size - 1; i >= 0; i--) {
            builder.PushElement(data[i]);
        }
        dataoffs = builder.EndVector(size);
    }

    auto packetBuilder = nier::PacketBuilder(builder);

    packetBuilder.add_magic(1347240270);
    packetBuilder.add_id(id);

    if (data != nullptr && size > 0) {
        packetBuilder.add_data(dataoffs);
    }

    builder.Finish(packetBuilder.Finish());

    this->send_packet(0, builder.GetBufferPointer(), builder.GetSize(), ENET_PACKET_FLAG_RELIABLE);
}

void NierClient::sendAnimationStart(uint32_t anim, uint32_t variant, uint32_t a3, uint32_t a4) {
    nier::AnimationStart data{anim, variant, a3, a4};

    flatbuffers::FlatBufferBuilder builder(0);
    auto dataoffs = builder.CreateStruct(data);
    builder.Finish(dataoffs);

    sendPacket(nier::PacketType_ID_ANIMATION_START, builder.GetBufferPointer(), builder.GetSize());
}

void NierClient::sendButtons(const uint32_t* buttons) {
    flatbuffers::FlatBufferBuilder builder(0);
    const auto dataoffs = builder.CreateVector(buttons, 8);

    nier::Buttons::Builder dataBuilder(builder);
    dataBuilder.add_buttons(dataoffs);
    builder.Finish(dataBuilder.Finish());

    sendPacket(nier::PacketType_ID_BUTTONS, builder.GetBufferPointer(), builder.GetSize());
}

void NierClient::sendHello() {
    auto ents = EntityList::get();
    auto possessed = ents->getPossessedEntity();

    if (possessed == nullptr || possessed->entity == nullptr) {
        spdlog::error("No possessed entity");
        return;
    }
    

    flatbuffers::FlatBufferBuilder builder{};
    const auto name_pkt = builder.CreateString(m_helloName);
    const auto pwd_pkt = builder.CreateString(m_password);

    nier::HelloBuilder helloBuilder(builder);
    helloBuilder.add_major(nier::VersionMajor_Value);
    helloBuilder.add_minor(nier::VersionMinor_Value);
    helloBuilder.add_patch(nier::VersionPatch_Value);
    helloBuilder.add_name(name_pkt);
    helloBuilder.add_password(pwd_pkt);
    helloBuilder.add_model(*possessed->entity->getModel());

    builder.Finish(helloBuilder.Finish());

    sendPacket(nier::PacketType_ID_HELLO, builder.GetBufferPointer(), builder.GetSize());
    m_helloSent = true;
}

void NierClient::updateLocalPlayerData() {
    if (!m_helloSent || !m_welcomeReceived || m_guid == 0) {
        return;
    }

    auto it = m_players.find(m_guid);

    if (it == m_players.end() || it->second == nullptr) {
        spdlog::error("Local player not set up");
        return;
    }

    auto entityList = EntityList::get();

    if (entityList == nullptr) {
        return;
    }

    auto player = entityList->getPossessedEntity();

    if (player == nullptr) {
        return;
    }

    it->second->setHandle(player->handle);
}

void NierClient::sendPlayerData() {
    if (m_guid == 0) {
        spdlog::error("Cannot send player data without GUID");
        return;
    }

    auto it = m_players.find(m_guid);

    if (it == m_players.end() || it->second == nullptr) {
        spdlog::error("Cannot send player data without player");
        return;
    }

    auto& player = it->second;
    
    auto entity = player->getEntity();

    if (entity == nullptr) {
        spdlog::error("Cannot send player data without entity");
        return;
    }

    nier::PlayerData playerData(
        *entity->getFlashlightEnabled(),
        *entity->getSpeed(),
        *entity->getFacing(),
        *entity->getFacing2(),
        *entity->getWeaponIndex(),
        *entity->getPodIndex(),
        entity->getCharacterController()->heldFlags,
        *(nier::Vector3f*)entity->getPosition()
    );

    flatbuffers::FlatBufferBuilder builder{};
    const auto offs = builder.CreateStruct(playerData);
    builder.Finish(offs);

    sendPacket(nier::PacketType_ID_PLAYER_DATA, builder.GetBufferPointer(), builder.GetSize());
}

bool NierClient::handleWelcome(const nier::Packet* packet) {
    spdlog::info("Welcome packet received");

    const auto welcome = flatbuffers::GetRoot<nier::Welcome>(packet->data()->data());
    auto verif = flatbuffers::Verifier(packet->data()->data(), packet->data()->size());

    if (!welcome->Verify(verif)) {
        spdlog::error("Invalid welcome packet");
        return false;
    }

    m_isMasterClient = welcome->isMasterClient();
    m_guid = welcome->guid();

    spdlog::info("Welcome packet received, isMasterClient: {}, guid: {}", m_isMasterClient, m_guid);

    return true;
}

bool NierClient::handleCreatePlayer(const nier::Packet* packet) {
    spdlog::info("Create player packet received");

    auto entityList = EntityList::get();

    if (entityList == nullptr) {
        spdlog::error("Entity list not found while handling create player packet");
        return false;
    }

    auto possessed = entityList->getPossessedEntity();

    if (possessed == nullptr) {
        spdlog::error("Possessed entity not found while handling create player packet");
        return false;
    }

    auto localplayer = entityList->getByName("Player");

    if (localplayer == nullptr || localplayer->entity == nullptr) {
        spdlog::info("Player not found while handling create player packet");
        return false;
    }

    const auto createPlayer = flatbuffers::GetRoot<nier::CreatePlayer>(packet->data()->data());
    auto verif = flatbuffers::Verifier(packet->data()->data(), packet->data()->size());

    if (!createPlayer->Verify(verif)) {
        spdlog::error("Invalid create player packet");
        return false;
    }

    {
        auto newPlayer = std::make_unique<Player>();
        newPlayer->setGuid(createPlayer->guid());
        newPlayer->setName(createPlayer->name()->c_str());

        m_players[createPlayer->guid()] = std::move(newPlayer);
    }

    // we don't want to spawn ourselves
    if (createPlayer->guid() != m_guid) {
        spdlog::info("Spawning player {}, {}", createPlayer->guid(), createPlayer->name()->c_str());

        auto ent = entityList->spawnEntity("partner", createPlayer->model(), *possessed->entity->getPosition());

        if (ent != nullptr) {
            spdlog::info(" Player spawned");

            ent->entity->setBuddyHandle(localplayer->handle);
            localplayer->entity->setBuddyHandle(ent->handle);

            ent->entity->setSuspend(false);

            ent->assignAIRoutine("PLAYER");
            ent->assignAIRoutine("player");

            // alternate way of assigning AI/control to the entity easily.
            localplayer->entity->changePlayer();
            localplayer->entity->changePlayer();

            const auto old_flags = ent->entity->getBuddyFlags();
            ent->entity->setBuddyFlags(-1);
            ent->entity->setBuddyFromNpc();
            ent->entity->setBuddyFlags(0);

            m_players[createPlayer->guid()]->setStartTick(*ent->entity->getTickCount());
            m_players[createPlayer->guid()]->setHandle(ent->handle);

            spdlog::info(" player assigned handle {:x}", ent->handle);
        } else {
            spdlog::error("Failed to spawn partner");
        }
    } else {
        spdlog::info("not spawning self");
    }

    return true;
}

bool NierClient::handleDestroyPlayer(const nier::Packet* packet) {
    spdlog::info("Destroy player packet received");

    const auto destroyPlayer = flatbuffers::GetRoot<nier::DestroyPlayer>(packet->data()->data());

    if (m_players.contains(destroyPlayer->guid()) && m_players[destroyPlayer->guid()] != nullptr) {
        auto entityList = EntityList::get();

        if (entityList == nullptr) {
            // not an error, we just won't actually delete any entity from the entity list
            spdlog::info("Entity list not found while handling destroy player packet");
        } else {
            auto localplayer = entityList->getByName("Player");
            auto ent = entityList->getByHandle(m_players[destroyPlayer->guid()]->getHandle());
            if (ent != nullptr && ent != localplayer) {
                ent->entity->terminate();
            }
        }
    }

    m_players[destroyPlayer->guid()].reset();
    m_players.erase(destroyPlayer->guid());

    return true;
}

bool NierClient::handlePlayerData(const nier::PlayerPacket* packet) {
    const auto guid = packet->guid();

    // do not update the local player. maybe change this later for forced updates/teleportation commands?
    if (guid == m_guid) {
        return true;
    }

    if (!m_players.contains(guid)) {
        spdlog::error("Player data packet received for unknown player {}", guid);
        return false;
    }

    const auto& playerNetworked = m_players[guid];

    if (playerNetworked == nullptr) {
        spdlog::error("(nullptr) Player data packet received for unknown player {}", guid);
        return false;
    }

    auto playerData = flatbuffers::GetRoot<nier::PlayerData>(packet->data()->data());
    auto npc = playerNetworked->getEntity();
    
    if (npc != nullptr) {
        *npc->getPosition() = *(Vector3f*)&playerData->position();
    }

    playerNetworked->setPlayerData(*playerData);

    return true;
}

bool NierClient::handleAnimationStart(const nier::PlayerPacket* packet) {
    const auto guid = packet->guid();

    // do not update the local player. maybe change this later for forced updates/teleportation commands?
    if (guid == m_guid) {
        return true;
    }

    if (!m_players.contains(guid)) {
        spdlog::error("Player data packet received for unknown player {}", guid);
        return false;
    }

    const auto& playerNetworked = m_players[guid];

    if (playerNetworked == nullptr) {
        spdlog::error("(nullptr) Player data packet received for unknown player {}", guid);
        return false;
    }

    auto animationData = flatbuffers::GetRoot<nier::AnimationStart>(packet->data()->data());
    auto npc = playerNetworked->getEntity();

    if (npc != nullptr) {
        switch (animationData->anim()) {
        case INVALID_CRASHES_GAME:
        case INVALID_CRASHES_GAME2:
        case INVALID_CRASHES_GAME3:
        case INVALID_CRASHES_GAME4:
        case Light_Attack:
            return true;
        default:
            if (npc) {
                npc->startAnimation(animationData->anim(), animationData->variant(), animationData->a3(), animationData->a4());
            } else {
                spdlog::error("Cannot start animation, npc is null");
            }
        }
    }
    
    return true;
}

bool NierClient::handleButtons(const nier::PlayerPacket* packet) {
    const auto guid = packet->guid();
    
    // do not update the local player. maybe change this later for forced updates/teleportation commands?
    if (guid == m_guid) {
        return true;
    }

    if (!m_players.contains(guid)) {
        spdlog::error("Player data packet received for unknown player {}", guid);
        return false;
    }

    const auto& playerNetworked = m_players[guid];

    if (playerNetworked == nullptr) {
        spdlog::error("(nullptr) Player data packet received for unknown player {}", guid);
        return false;
    }

    auto buttons = flatbuffers::GetRoot<nier::Buttons>(packet->data()->data());
    auto npc = playerNetworked->getEntity();

    if (npc != nullptr) {
        const auto buttonsData = buttons->buttons()->data();
        const auto sizeButtons = sizeof(Entity::CharacterController::buttons);
        memcpy(&npc->getCharacterController()->buttons, buttonsData, sizeButtons);

        for (uint32_t i = 0; i < Entity::CharacterController::INDEX_MAX; ++i) {
            auto controller = npc->getCharacterController();

            if (buttonsData[i] > 0) {
                controller->heldFlags |= (1 << i);
            }
        }
    }

    return true;
}
