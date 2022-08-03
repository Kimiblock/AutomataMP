package main

import (
	nier "automatampserver/nier"
	"math/rand"
	"time"

	"github.com/codecat/go-enet"
	"github.com/codecat/go-libs/log"
	flatbuffers "github.com/google/flatbuffers/go"
)

type Player struct {
	guid           uint64
	name           string
	model          uint32
	lastPlayerData *nier.PlayerData
}

// create guid->player map
var players = make(map[uint64]*Player)

type LocalPlayer struct {
	isMasterClient bool
	guid           uint64
	player         *Player
}

var localPlayer *LocalPlayer = nil

func checkValidPacket(data *nier.Packet) bool {
	if data.Magic() != 1347240270 {
		log.Error("Invalid magic number: %d", data.Magic())
		return false
	}

	if data.Id() == 0 {
		log.Error("Invalid packet type: %d", data.Id())
		return false
	}

	return true
}

func packetStart(id nier.PacketType) *flatbuffers.Builder {
	builder := flatbuffers.NewBuilder(0)
	nier.PacketStart(builder)
	nier.PacketAddMagic(builder, 1347240270)
	nier.PacketAddId(builder, id)
	//nier.PacketEnd(builder)

	return builder
}

func packetStartWithData(id nier.PacketType, data []uint8) *flatbuffers.Builder {
	builder := flatbuffers.NewBuilder(0)

	dataoffs := flatbuffers.UOffsetT(0)

	if len(data) > 0 {
		nier.PacketStartDataVector(builder, len(data))
		for i := len(data) - 1; i >= 0; i-- {
			builder.PrependUint8(data[i])
		}
		dataoffs = builder.EndVector(len(data))
	}

	nier.PacketStart(builder)
	nier.PacketAddMagic(builder, 1347240270)
	nier.PacketAddId(builder, id)

	if (len(data)) > 0 {
		nier.PacketAddData(builder, dataoffs)
	}
	//nier.PacketEnd(builder)

	return builder
}

func makePacketBytes(id nier.PacketType, data []uint8) []uint8 {
	builder := packetStartWithData(id, data)
	builder.Finish(nier.PacketEnd(builder))
	return builder.FinishedBytes()
}

func makeEmptyPacketBytes(id nier.PacketType) []uint8 {
	builder := packetStart(id)
	builder.Finish(nier.PacketEnd(builder))
	return builder.FinishedBytes()
}

func builderSurround(cb func(*flatbuffers.Builder) flatbuffers.UOffsetT) []uint8 {
	builder := flatbuffers.NewBuilder(0)
	offs := cb(builder)
	builder.Finish(offs)
	return builder.FinishedBytes()
}

func sendPing(peer enet.Peer) {
	peer.SendBytes(makeEmptyPacketBytes(nier.PacketTypeID_PING), 0, enet.PacketFlagReliable)
}

func sendHello(peer enet.Peer, name string, password string) {
	helloBytes := builderSurround(func(builder *flatbuffers.Builder) flatbuffers.UOffsetT {
		name_pkt := builder.CreateString(name)
		pwd_pkt := builder.CreateString(password)
		nier.HelloStart(builder)
		nier.HelloAddMajor(builder, uint32(nier.VersionMajorValue))
		nier.HelloAddMinor(builder, uint32(nier.VersionMinorValue))
		nier.HelloAddPatch(builder, uint32(nier.VersionPatchValue))
		nier.HelloAddName(builder, name_pkt)
		nier.HelloAddPassword(builder, pwd_pkt)

		return nier.HelloEnd(builder)
	})

	pkt := makePacketBytes(nier.PacketTypeID_HELLO, helloBytes)
	peer.SendBytes(pkt, 0, enet.PacketFlagReliable)
}

func getNextPacket(ev enet.Event) *nier.Packet {
	if ev.GetType() == enet.EventReceive {
		packet := ev.GetPacket()
		defer packet.Destroy()

		// Get the bytes in the packet
		packetBytes := packet.GetData()

		log.Info("Received %d bytes from server", len(packetBytes))

		data := nier.GetRootAsPacket(packetBytes, 0)

		if !checkValidPacket(data) {
			return nil
		}

		return data
	}

	return nil
}

func sendHelloAndWait(client enet.Host, peer enet.Peer) bool {
	sendHello(peer, "", "test")

	for i := 0; i < 20; i++ {
		ev := client.Service(100)
		data := getNextPacket(ev)

		if data == nil {
			continue
		}

		if data.Id() == nier.PacketTypeID_WELCOME {
			log.Info("Hello acknowledged")
			welcome := nier.GetRootAsWelcome(data.DataBytes(), 0)

			localPlayer = new(LocalPlayer)
			localPlayer.isMasterClient = welcome.IsMasterClient()
			localPlayer.guid = welcome.Guid()

			log.Info("Welcome: %d", welcome.Guid())
			log.Info("Is master client: %t", welcome.IsMasterClient())

			return true
		}
	}

	return false
}

func performStartupHandshake(client enet.Host, peer enet.Peer) bool {
	log.Info("Performing startup handshake")

	hasConnection := false

	for i := 0; i < 10; i++ {
		ev := client.Service(1000)

		if ev.GetType() == enet.EventConnect {
			log.Info("Intial connection established")
			hasConnection = true
			break
		}

		if ev.GetType() == enet.EventDisconnect {
			log.Error("Initial connection failed")
			return false
		}
	}

	if !hasConnection {
		log.Error("Initial connection failed")
		return false
	}

	log.Info("Sending initial hello...")
	if !sendHelloAndWait(client, peer) {
		log.Error("Failed to receive hello response from server")
		return false
	}

	log.Info("Connected.")

	return true
}

func spawnPlayer(player *Player) {
	if player.guid == localPlayer.guid {
		localPlayer.player = player
	}

	log.Info(" Spawning player %d", player.guid)
	log.Info(" Is local player: %t", player.guid == localPlayer.guid)
	log.Info(" Is master client: %t", player.guid == localPlayer.guid && localPlayer.isMasterClient)
	log.Info(" Model: %d", player.model)

	// Doesn't actually do anything, this is just mock code.
	// Implement it in the actual game.
}

func main() {
	// Initialize enet
	enet.Initialize()

	// Create a client host
	client, err := enet.NewHost(nil, 1, 1, 0, 0)
	if err != nil {
		log.Error("Couldn't create host: %s", err.Error())
		return
	}

	// Connect the client host to the server
	peer, err := client.Connect(enet.NewAddress("127.0.0.1", 6969), 1, 0)
	if err != nil {
		log.Error("Couldn't connect: %s", err.Error())
		return
	}

	if !performStartupHandshake(client, peer) {
		return
	}

	pingTime := time.Now()
	sendUpdateTime := time.Now()
	once_test := true

	// The event loop
	for true {
		now := time.Now()

		// Wait until the next event
		ev := client.Service(1000 / 60)

		// Send a ping if we didn't get any event
		if ev.GetType() == enet.EventNone {
			if once_test {
				log.Info("Sending animation start")
				animationStartBytes := builderSurround(func(builder *flatbuffers.Builder) flatbuffers.UOffsetT {
					return nier.CreateAnimationStart(builder, 1, 2, 3, 4)
				})

				animationData := makePacketBytes(nier.PacketTypeID_ANIMATION_START, animationStartBytes)
				peer.SendBytes(animationData, 0, enet.PacketFlagReliable)

				once_test = false
			}

			if now.Sub(pingTime) > time.Second {
				log.Info("Sending ping")

				sendPing(peer)
				pingTime = now
				continue
			}

			if now.Sub(sendUpdateTime) >= (time.Second / 60) {
				//log.Info("Sending update")

				playerDataBytes := builderSurround(func(builder *flatbuffers.Builder) flatbuffers.UOffsetT {
					return nier.CreatePlayerData(builder, true, 0.1, 0.5, 0.25, 1, 0, 0, rand.Float32(), rand.Float32(), 250.0)
				})

				packetData := makePacketBytes(nier.PacketTypeID_PLAYER_DATA, playerDataBytes)
				peer.SendBytes(packetData, 0, enet.PacketFlagReliable)
				sendUpdateTime = now
				continue
			}
		}

		switch ev.GetType() {
		case enet.EventConnect: // We connected to the server
			log.Info("Connected to the server!")

		case enet.EventDisconnect: // We disconnected from the server
			log.Info("Lost connection to the server!")

		case enet.EventReceive: // The server sent us data
			packet := ev.GetPacket()
			defer packet.Destroy()

			// Get the bytes in the packet
			packetBytes := packet.GetData()

			log.Info("Received %d bytes from server", len(packetBytes))

			data := nier.GetRootAsPacket(packetBytes, 0)

			if !checkValidPacket(data) {
				continue
			}

			var playerPacket *nier.PlayerPacket = nil

			// Bounced player packets from server
			if data.Id() > nier.PacketTypeID_CLIENT_START && data.Id() < nier.PacketTypeID_CLIENT_END {
				log.Info("Bounced packet %d received", data.Id())
				playerPacket = nier.GetRootAsPlayerPacket(data.DataBytes(), 0)
			}

			switch data.Id() {
			case nier.PacketTypeID_PONG:
				log.Info("Pong received")
				break

			case nier.PacketTypeID_CREATE_PLAYER:
				log.Info("Player creation packet received")

				playerCreationPacket := nier.GetRootAsCreatePlayer(data.DataBytes(), 0)

				players[playerCreationPacket.Guid()] = new(Player)
				players[playerCreationPacket.Guid()].guid = playerCreationPacket.Guid()
				players[playerCreationPacket.Guid()].name = string(playerCreationPacket.Name())
				players[playerCreationPacket.Guid()].model = playerCreationPacket.Model()

				// Doesn't actually do anything, this is just mock code.
				// implement it in the actual game.
				spawnPlayer(players[playerCreationPacket.Guid()])

				break
			case nier.PacketTypeID_DESTROY_PLAYER:
				log.Info("Player destruction packet received")

				playerDestructionPacket := &nier.DestroyPlayer{}
				flatbuffers.GetRootAs(data.DataBytes(), 0, playerDestructionPacket)

				log.Info(" Destroying player %d", playerDestructionPacket.Guid())

				players[playerDestructionPacket.Guid()] = nil // ezpz

				break
			// Bounced packets from server
			case nier.PacketTypeID_ANIMATION_START:
				log.Info("Animation start received from client %d", playerPacket.Guid())

				if players[playerPacket.Guid()] == nil {
					log.Error(" Player %d not found", playerPacket.Guid())
					continue
				}

				animationData := &nier.AnimationStart{}
				flatbuffers.GetRootAs(playerPacket.DataBytes(), 0, animationData)

				log.Info(" Animation: %d", animationData.Anim())
				log.Info(" Variant: %d", animationData.Variant())
				log.Info(" a3: %d", animationData.A3())
				log.Info(" a4: %d", animationData.A4())

				break
			case nier.PacketTypeID_PLAYER_DATA:
				log.Info("Player data received from client %d", playerPacket.Guid())

				if players[playerPacket.Guid()] == nil {
					log.Error(" Player %d not found", playerPacket.Guid())
					continue
				}

				playerData := &nier.PlayerData{}
				flatbuffers.GetRootAs(playerPacket.DataBytes(), 0, playerData)

				log.Info("Flashlight: %d", playerData.Flashlight())
				log.Info("Speed: %f", playerData.Speed())
				log.Info("Facing: %f", playerData.Facing())
				pos := playerData.Position(nil)
				log.Info("Position: %f, %f, %f", pos.X(), pos.Y(), pos.Z())

				break
			default:
				log.Error("Unknown packet type: %d", data.Id())
			}
		}
	}

	// Destroy the host when we're done with it
	client.Destroy()

	// Uninitialize enet
	enet.Deinitialize()
}
