#include "pch-il2cpp.h"
#include "Server.h"

#include "Chat.h"
#include "Mod.h"
#include "Weapon.h"
#include "Config.h"
#include "PermissionGroups.h"
#include "systems/ModeDeathMatch.h"
#include "systems/EquipItem.h"
#include "systems/UpdateCheck.h"
#include "systems/AutoStart.h"
#include "systems/Fly.h"
#include "systems/VoteSystem.h"
#include "systems/Hover.h"
#include "systems/MapSkip.h"
#include "systems/Whitelist.h"
#include "systems/AutoMessages.h"
#include "templates/templates.h"
#include "Message.h"

std::map<long long, Player*> Server::Players;

int Server::MapId = -1;
int Server::MapModeId = -1;
long long Server::LobbyId = 0;

sio::client Server::SIOClient;
bool Server::AutoKillHostOnGameStart = false;
bool Server::AutoReadyHostOnLobby = false;
bool Server::ShowJoinLeaveMessages = false;

int Server::PunchDamageId = -1;

std::vector<Message*> Server::MessageQueue = {};


void on_connection_open(void) {
	std::cout << "Connection open" << std::endl;
}

void on_connection_close(int const& reason) 
{
	std::cout << "Connection close" << std::endl;
}

void on_socket_connection_open(std::string const &nsp) 
{
	std::cout << "Socket Connection open on " << nsp << std::endl;
}

void on_socket_connection_close(std::string const &nsp) 
{
	std::cout << "Socket Connection close" << nsp << std::endl;
}

void OnRemoteCommand(sio::event& event) 
{
	auto flag = event.get_message().get()->get_flag();
	if (flag != sio::message::flag_string)
	{
		std::cout << "[SERVER] Expected a string command" << std::endl;
		return;
	}

	auto remote_message = event.get_message().get()->get_string();

	std::cout << "[Server] Forwarding remote command " << remote_message << std::endl;
	// DO NOT EXECUTE THE COMMAND HERE!
	// This listener executes on a different thread. Almost everything dealing with IL2CPP
	// calls completely break when executing on this thread, presumably because the IL2CPP
	// code expects to be run on a different thread.
	// Therefore, we receive the message by queueing it and executing it on the correct
	// thread later, in the Server::Update function.
	Message* message = new Message(Server::GetLobbyOwner(), remote_message);
	Server::MessageQueue.push_back(message);
}

void OnPollState(sio::event& event) 
{
	Server::EmitState();
}

void Server::Init()
{
	std::cout << "[Server] Init" << std::endl;

	// Server::SIOClient.set_logs_verbose();
	Server::SIOClient.connect("http://localhost:3000");
	Server::SIOClient.socket()->emit("ping");
	Server::SIOClient.socket()->on("remote_command", &OnRemoteCommand);
	Server::SIOClient.socket()->on("poll_state", &OnPollState);
	Config::Load();

	Chat::Init();

	std::cout << "[Info] You can type '!config reload' to reload config.ini" << std::endl;
}


float Server::EmitStateInterval = 5.0f;
float Server::EmitStateTimeElapsed = 0;

void Server::EmitState() 
{
	// Too noisy
	// std::cout << "[Server] Emitting state" << std::endl;

	Json::Value state = Json::objectValue;

	Json::Value playersValue = Json::objectValue;
	for (auto pair : Server::Players)
	{
		auto key = pair.first;
		auto player = pair.second;

		Json::Value playerValue;
		playerValue["Username"] = player->Username;
		playerValue["PermissionGroupId"] = player->PermissionGroupId;
		// such a large number doesn't serialize to JS numbers well
		playerValue["ClientId"] = std::to_string(player->ClientId);
		playerValue["IsOnline"] = player->IsOnline;
		playerValue["IsAlive"] = player->IsAlive;
		playerValue["SpawnedThisRound"] = player->SpawnedThisRound;
		playerValue["DiedThisRound"] = player->DiedThisRound;
		playerValue["FirstEverSpawned"] = player->FirstEverSpawn;
		playerValue["Godmode"] = player->Godmode;
		playerValue["JumpPunchEnabled"] = player->JumpPunchEnabled;
		playerValue["SuperPunchEnabled"] = player->SuperPunchEnabled;
		playerValue["ForceFieldEnabled"] = player->ForceFieldEnabled;
		playerValue["MultiSnowballEnabled"] = player->MultiSnowballEnabled;
		playerValue["MuteTime"] = player->MuteTime;
		playerValue["RespawnTime"] = player->RespawnTime;


		playersValue[std::to_string(player->ClientId)] = playerValue;
	}

	state["Players"] = playersValue;

	Server::SIOClient.socket()->emit("state_update", state.toStyledString());
}

void Server::ProcessEmitState(float dt)
{
	EmitStateTimeElapsed += dt;
	if (EmitStateTimeElapsed >= EmitStateInterval)
	{
		std::cout << "[Server] Emitting state" << std::endl;
		EmitStateTimeElapsed = 0;
		EmitState();
	}
}

void Server::Update(float dt)
{
	if (!Mod::IsInAnyLobby()) return;

	//std::cout << "[Server] owner " << Mod::GetCurrentLobbyOwnerId() << " | lobby " << LobbyId << " | myid " << Mod::GetMySteamId() << std::endl;

	EquipItem::Update();

	if (!Mod::IsLobbyOwner()) return;

	UpdatePlayersPosition();

	AutoMessages::Update(dt);
	VoteSystem::Update(dt);
	UpdateCheck::Check();
	ModeDeathMatch::Update(dt);
	AutoStart::Update(dt);
	Fly::Update(dt);
	Hover::Update(dt);
	BanSystem::Update(dt);

	//ProcessSpawnRequest
	for (auto pair : Players)
	{
		auto player = pair.second;

		if (!player->SpawnCallbackRequest) continue;
		player->SpawnCallbackRequest = false;

		Server::OnPlayerSpawn(player, player->Position);
	}

	//ProcessRespawn
	for (auto pair : Players)
	{
		auto player = pair.second;

		if (player->RespawnTime > 0)
		{
			player->RespawnTime -= dt;
			if (player->RespawnTime < 0)
			{
				player->RespawnTime = 0;
				RespawnPlayer(player);
			}
		}
	}

	while (!Server::MessageQueue.empty())
	{
		auto message = std::move(Server::MessageQueue.front());
		Server::MessageQueue.erase(Server::MessageQueue.begin());
		Chat::AddMessageAndProcess(message);
	}

	Chat::Update(dt);

	Config::ProcessAutoSave(dt);

	//banned players is handled on LobbyManager_OnPlayerJoinLeaveUpdate
	if (!Mod::ConsoleMode)
	{
		auto bannedPlayers = (*LobbyManager__TypeInfo)->static_fields->bannedPlayers;
		List_1_System_UInt64__Clear(bannedPlayers, NULL);
	}

	Server::ProcessEmitState(dt);
}

void Server::UpdatePlayersPosition()
{
	/*
	for (auto pair : m_Players)
	{
		auto player = pair.second;
		player->m_PlayerManager = NULL;
	}
	*/

	if (Mod::ConsoleMode) return;
	//if (!*GameManager__TypeInfo) return;

	auto gameManager = (*GameManager__TypeInfo)->static_fields->Instance;

	if (!gameManager) return;

	auto activePlayers = gameManager->fields.activePlayers;

	for (size_t i = 0; i < activePlayers->fields.count; i++)
	{
		auto key = activePlayers->fields.entries->vector[i].key;
		auto playerManager = activePlayers->fields.entries->vector[i].value;

		if (!playerManager) continue;

		auto player = GetPlayer(key);

		if (!player) return;

		//player->PlayerManager = playerManager;

		auto transform = Component_get_transform((Component*)playerManager, nullptr);
		auto pos = Transform_get_position(transform, nullptr);

		if (!player->FlyEnabled) //otherwise players will fall slowly
			player->Position = pos;

		//auto headTransform = playerManager->fields.head;
		//auto headPos = Transform_get_position(headTransform, nullptr);
		//std::cout << "headPos" << headPos.toString() << std::endl;

		//auto euler = Transform_get_eulerAngles(headTransform, nullptr);
		//std::cout << "head euler" << euler.toString() << std::endl;

		//std::cout << "head forward" << player->m_FlyDir.toString() << std::endl;

		auto quat = PlayerManager_GetRotation(playerManager, NULL); //thank god (or thank Dani) this function exists lol
		auto forward = Quaternion_op_Multiply_1(quat, Vector3_get_forward(NULL), NULL);
		player->LookDir = Vector3_Normalize(forward, NULL);

		//Quaternion__Boxed* myQuat = (Quaternion__Boxed*)il2cpp_object_new((Il2CppClass*)*Quaternion__TypeInfo);
		//Quaternion__ctor(myQuat, quat.x, quat.y, quat.z, quat.w, NULL);


		/*
		auto quatEuler = Quaternion_get_eulerAngles(myQuat, NULL);
		auto pitch = -quatEuler.y * (M_PI / 180.0);
		auto yaw = quatEuler.x * (M_PI / 180.0);
		Vector3 direction(
			std::cos(yaw) * std::cos(pitch),
			std::sin(yaw) * std::cos(pitch),
			std::sin(pitch)
		);
		std::cout << "quatEuler" << quatEuler.toString() << std::endl;
		std::cout << "direction" << direction.toString() << std::endl;
		*/

		//Quaternion_

		//auto quatEuler = Quaternion_get_eulerAngles(quat, NULL);

		//boxed

		//targetRot * Vector3.forward;

		//Quaternion_iden


		//player->m_FlyDir = euler;

		// 
		//auto headTransform = playerManager->fields.head;
		//auto headPos = Transform_get_position(headTransform, nullptr);

		//std::cout << i << " : " << key << ", dead=" << playerManager->fields.dead << ", justdied=" << playerManager->fields.justDied << std::endl;
		//std::cout << key << " transform pos" << pos.x << ", " << pos.y << ", " << pos.z << std::endl;
		//std::cout << "head pos" << headPos.x << ", " << headPos.y << ", " << headPos.z << std::endl;

		//std::cout << key << " transform pos" << Mod::FormatVector(GetPlayer(key)->m_Position) << std::endl;
	}
}

bool Server::HasPlayer(long long clientId)
{
	return Players.find(clientId) != Players.end();
}

Player* Server::GetPlayer(long long clientId)
{
	if (!HasPlayer(clientId)) return NULL;
	return Players.at(clientId);
}

Player* Server::AddPlayer(Player* player)
{
	std::cout << "[Server] AddPlayer " << player->GetDisplayNameExtra() << std::endl;

	Players.insert(std::pair<long long, Player*>(player->ClientId, player));
	return player;
}

void Server::RemovePlayer(Player* player)
{
	std::cout << "[Server] RemovePlayer " << player->GetDisplayNameExtra() << std::endl;

	Players.erase(player->ClientId);
	delete player;
}

std::vector<Player*> Server::GetPlayers()
{
	std::vector<Player*> players;
	for (auto pair : Players)
	{
		auto player = pair.second;
		players.push_back(player);
	}
	return players;
}

std::vector<Player*> Server::GetOnlinePlayers()
{
	std::vector<Player*> players;
	auto allPlayers = GetPlayers();

	for (auto player : allPlayers)
	{
		if (!player->IsOnline) continue;
		players.push_back(player);
	}
	return players;
}

void Server::OnAddPlayerToLobby(long long clientId)
{
	bool firstJoin = !HasPlayer(clientId);

	if (firstJoin)
	{
		auto newPlayer = new Player(clientId);
		newPlayer->UpdateInfo();

		AddPlayer(newPlayer);
	}

	auto player = GetPlayer(clientId);
	player->UpdateInfo();
	player->IsOnline = true;

	OnPlayerJoin(player);

	if(firstJoin) OnPlayerFirstJoin(player);

	Server::EmitState();
}

void Server::OnRemovePlayerFromLobby(long long clientId)
{
	auto player = GetPlayer(clientId);
	player->IsOnline = false;

	OnPlayerLeave(player);

	Server::EmitState();
}

void Server::OnPlayerFirstJoin(Player* player)
{
	if (Server::ShowJoinLeaveMessages)
		std::cout << "[Server] " << player->GetDisplayNameExtra() << " joined for the first time" << std::endl;

	player->FirstEverSpawn = true;

	//if (player->IsLobbyOwner()) player->PermissionGroupId = "admin";
	//lobby owner hasnt been updated yet :/

	Config::SavePlayers();

	Server::EmitState();
}

void Server::OnPlayerJoin(Player* player)
{
	if (Server::ShowJoinLeaveMessages)
		std::cout << "[Server] " << player->GetDisplayNameExtra() << " joined" << std::endl;

	player->IsOnline = true;
	player->SpawnedThisRound = false; //alreay added at OnLeave
	player->DiedThisRound = false; //alreay added at OnLeave

	//alreay added at OnLeave
	//if he spawns (lobby example), alive is set to true
	//if this is set to true, it will spawn even if game is already running YEY
	player->IsAlive = false;

	ModeDeathMatch::OnPlayerJoin(player);

	Server::EmitState();
}

void Server::OnPlayerLeave(Player* player)
{
	if (Server::ShowJoinLeaveMessages)
		std::cout << "[Server] " << player->GetDisplayNameExtra() << " left" << std::endl;

	player->Client = NULL;
	player->PlayerManager = NULL;
	player->IsAlive = false;
	player->Id = -1;
	player->IsOnline = false;
	player->SpawnedThisRound = false;
	player->DiedThisRound = false;

	Server::EmitState();
}

/*
Called when game sends an info that someone spawned
Can be called multiple times, so need to check its first time with .SpawnedThisRound
*/
void Server::OnGameSpawnPlayer(Player* player, Vector3 position)
{
	player->UpdateInfo();

	if (player->SpawnedThisRound)
	{
		//Chat::SendServerMessage(spawnedPlayer->GetDisplayNameExtra() + " already spawned, ignoring");
		return;
	}

	player->SpawnedThisRound = true;
	player->SpawnCallbackRequest = true;
	//Server::OnPlayerSpawn(player, player->Positon);

	Server::EmitState();
}

bool Server::OnPlayerTryToSpawnSpectator(Player* player)
{
	player->UpdateInfo();

	std::cout << "[Server] OnPlayerTryToSpawnSpectator: " << player->GetDisplayNameExtra() << std::endl;
	std::cout << "         IsAlive= " << (player->IsAlive ? "TRUE" : "FALSE") << std::endl;

	//wants to spawn spectator but IsAlive is set to true
	if (player->IsAlive)
	{
		if (!player->Client)
		{
			Chat::SendServerMessage("OnPlayerTryToSpawnSpectator: Client not found");
			return true;
		}

		auto byteArray = player->Client->fields.u109Au109Au10A1u109Au109Bu10A2u10A6u10A2u1099u109Bu10A7;
		auto numberId = player->Client->fields.u1099u109Au10A1u1099u10A8u109Eu10A0u10A0u109Eu109Au10A0;

		std::cout << "[Server] Sending GameServer_PlayerSpawnRequest byteArray=" << byteArray << ", numberId=" << numberId << std::endl;

		GameServer_PlayerSpawnRequest(
			player->ClientId,
			false,
			byteArray,
			numberId,
			NULL
		);

		//Chat::SendServerMessage("[try-spawn-spec] alive=true, spawning in game");

		return false;
	}

	//Chat::SendServerMessage("[try-spawn-spec] spawning spectator");

	std::cout << "[Server] Spawning spec" << std::endl;

	//player->PlayerManager = NULL; //why did I add this?

	Server::EmitState();

	return true;
}


/*
* Called after player spawns
*/
void Server::OnPlayerSpawn(Player* player, Vector3 position)
{
	Server::SIOClient.socket()->emit("player_spawn", player->Username);
	player->IsAlive = true;

	std::cout << "[Server] OnPlayerSpawn " << player->GetDisplayNameExtra() << ", at " << position << std::endl;

	//Chat::SendServerMessage(player->GetDisplayNameExtra() + " spawned at " + position.format_3());

	if (player->FirstEverSpawn)
	{
		player->FirstEverSpawn = false;

		if (player->IsLobbyOwner()) player->PermissionGroupId = "admin";

		//Chat::SendServerMessage(player->GetDisplayName() + " is new here :)");
	}

	if (player == Server::GetLobbyOwner())
	{
		if (Server::IsAtLobby())
		{
			if (Server::AutoReadyHostOnLobby)
			{
				Mod::SetPlayerReady(player->ClientId, true);
				Mod::SendReadyInteract();
			}
		}
		else {
			if (Server::AutoKillHostOnGameStart)
			{
				Server::KillPlayer(player);
				return;
			}
		}

		
	}

	ModeDeathMatch::OnPlayerSpawn(player, position);
	Server::EmitState();
}

/*
return true - player dies
return false - player stays alive
*/
bool Server::OnPlayerDied(Player* deadPlayer, Player* killerPlayer, Vector3 damageDir)
{
	Server::SIOClient.socket()->emit("player_died", deadPlayer->Username);
	if (deadPlayer->Godmode)
		return false;

	deadPlayer->IsAlive = false;
	deadPlayer->DiedThisRound = true;

	if (Chat::ShowDeathMessages)
	{
		if (deadPlayer->ClientId == killerPlayer->ClientId)
		{
			Chat::SendServerMessage("" + deadPlayer->GetDisplayName() + " died");
			std::cout << "[Server] " << deadPlayer->GetDisplayNameExtra() << " died" << std::endl;
		}
		else
		{
			Chat::SendServerMessage("" + killerPlayer->GetDisplayName() + " killed " + deadPlayer->GetDisplayName());
			std::cout << "[Server] " << killerPlayer->GetDisplayNameExtra() << " killed " << deadPlayer->GetDisplayNameExtra() << std::endl;

		}
	}

	ModeDeathMatch::OnPlayerDied(deadPlayer, killerPlayer, damageDir);

	Server::EmitState();
	return true;
}

void Server::KillPlayer(Player* player)
{
	std::cout << "[Server] KillPlayer " << player->GetDisplayNameExtra() << std::endl;

	if (MapModeId == 4 || MapModeId == 5 || MapModeId == 6)
	{
		RespawnActivePlayerAtPos(player->ClientId, Vector3(0, -1000, 0));
		return;
	}

	GameServer_PlayerDied(player->ClientId, player->ClientId, Vector3({ 0, 1, 0 }), NULL);
	//ServerSend_PlayerDied(clientId, clientId, Vector3({ 0, 1, 0 }), NULL);
}


/*
Wtf, actually fixed?
Bug: If host respawns with load, he won't see the players that were alive
*/
void Server::RespawnPlayer(Player* player)
{
	std::cout << "[Mod] RespawnPlayer " << player->GetDisplayNameExtra() << std::endl;

	//Chat::SendServerMessage("[respawn] trying to respawn");

	//not alive and not already spawned, usually when spawning as spec in next game, or when join already started game
	if (!player->IsAlive && !player->DiedThisRound)
	{
		//Chat::SendServerMessage("[respawn] send load game");

		RespawnSpectator(player);
	}
	else {
		auto gameServer = (*GameServer__TypeInfo)->static_fields->Instance;
		GameServer_QueueRespawn(gameServer, player->ClientId, 0.0f, NULL);

		Server::OnPlayerSpawn(player, player->Position);
	}

	player->IsAlive = true;

	//update playermanager
	player->PlayerManager = NULL;
	player->UpdateInfo();
}

/*
Old respawn, or now a set position/tp
*/
void Server::RespawnActivePlayerAtPos(long long clientId, Vector3 position)
{
	if (Server::HasPlayer(clientId))
	{
		//TESTHERE

		auto player = Server::GetPlayer(clientId);
		player->IsAlive = true;
		player->Position = position;

		//std::cout << "[Server] RespawnActivePlayerAtPos " << player->GetDisplayNameExtra() << std::endl;
	}

	//std::cout << "[Mod] RespawnPlayer clientId=" << clientId << formatVector3_full(position) << std::endl;

	ServerSend_RespawnPlayer(clientId, position, NULL);
}

void Server::RespawnSpectator(Player* player)
{
	std::cout << "[Server] RespawnSpectator " << player->GetDisplayNameExtra() << std::endl;

	//to get client
	player->UpdateInfo();

	if (!player->Client)
	{
		Chat::SendServerMessage("RespawnSpectator: client not found");
		return;
	}

	ServerSend_LoadMap(Server::MapId, Server::MapModeId, player->ClientId, NULL);
	ServerSend_LoadingSendIntoGame(player->ClientId, NULL);

	GameServer_PlayerSpawnRequest(
		player->ClientId,
		false,
		player->Client->fields.u109Au109Au10A1u109Au109Bu10A2u10A6u10A2u1099u109Bu10A7, //byteArray
		player->Client->fields.u1099u109Au10A1u1099u10A8u109Eu10A0u10A0u109Eu109Au10A0, //numberId
		NULL
	);
}

std::vector<Player*> Server::FindPlayers(std::string selector)
{
	//add check for online

	std::vector<Player*> players;

	for (auto player : GetPlayers())
	{
		//contains only digits - "23" or "76561199219991380"
		if (std::all_of(selector.begin(), selector.end(), ::isdigit))
		{
			auto id = std::stoll(selector);

			// "76561199219991380" - dont need to be online
			if (player->ClientId == id)
			{
				players.push_back(player);
			}

			// "23"
			if (player->Id == id)
			{
				if (!player->IsOnline) continue;

				players.push_back(player);
			}
			continue;
		}

		if (!player->IsOnline) continue;

		// all - "*"
		if (selector == "*")
		{
			players.push_back(player);
			continue;
		}

		// all alive - "*a"
		if (selector == "*a")
		{
			if (!player->IsAlive) continue;

			players.push_back(player);
			continue;
		}

		// all dead - "*d"
		if (selector == "*d")
		{
			if (player->IsAlive) continue;

			players.push_back(player);
			continue;
		}

		// by id "#23"
		if (selector.rfind("#", 0) == 0)
		{
			std::string idstr;
			std::remove_copy(selector.begin(), selector.end(), std::back_inserter(idstr), '#');

			int id = std::atoi(idstr.c_str());

			if (player->Id == id)
			{
				players.push_back(player);
			}

			continue;
		}

		// any string
		std::string str1 = selector;
		std::transform(str1.begin(), str1.end(), str1.begin(), [](unsigned char c) { return std::tolower(c); });
		std::string str2 = player->Username;
		std::transform(str2.begin(), str2.end(), str2.begin(), [](unsigned char c) { return std::tolower(c); });

		if (str2.find(str1) != std::string::npos) {
			players.push_back(player);
		}
	}

	return players;
}

void Server::GiveWeapon(Player* player, int weaponId)
{
	std::cout << "[Server] GiveWeapon " << weaponId << " to " << player->GetDisplayNameExtra() << std::endl;

	//if (!player->IsAlive) return;

	Mod::ForceGiveItem(player->ClientId, weaponId, Mod::UniqueObjectId++);
}

void Server::DropWeapon(Player* player, int weaponId, int ammo)
{
	std::cout << "[Server] DropWeapon " << weaponId << " to " << player->GetDisplayNameExtra() << std::endl;

	//if (!player->IsAlive) return;

	//Mod::ForceGiveItem(player->ClientId, weaponId, Mod::UniqueObjectId++);
	Mod::SendDropItem(player->ClientId, weaponId, Mod::UniqueObjectId++, ammo);
}

void Server::OnMapLoad(int map, int mode)
{
	std::cout << "[Server] OnMapLoad" << std::endl;

	MapId = map;
	MapModeId = mode;

	AutoStart::OnMapLoad(map, mode);
	MapSkip::OnMapLoad(map, mode);
}

void Server::OnMapStart()
{
	std::cout << "[Server] OnMapStart" << std::endl;

	for (auto player : GetOnlinePlayers())
	{
		player->DiedThisRound = false;
		player->SpawnedThisRound = false;
	}

	GameServer_ForceRemoveAllWeapons(NULL);

	AutoStart::OnMapStart();
	Server::EmitState();
}

void Server::OnLobbyStart(long long lobbyId)
{
	std::cout << "[Server] OnLobbyStart " << lobbyId << std::endl;

	LobbyId = lobbyId;
	Server::EmitState();
}

void Server::RestartGame()
{
	Mod::RestartGame();

	OnMapLoad(MapId, MapModeId);
	//OnMapStart(); ///gets called automatically by game
	Server::EmitState();
}

void Server::OnPunchPlayer(uint64_t playerId, uint64_t punchedPlayerId, Vector3 dir, MethodInfo* method)
{
	//HOW TF DOES THIS CRASH
	//normal punch
	//HF_ServerSend_PunchPlayer->original(playerId, punchedPlayerId, dir, method);

	if (Server::HasPlayer(playerId)) Fly::OnPunch(Server::GetPlayer(playerId));
	Server::EmitState();
}

bool Server::IsAtLobby()
{
	return MapModeId == 0;
}

Player* Server::GetLobbyOwner()
{
	for (auto player : GetPlayers())
	{
		if (player->Id == 1) return player;
	}
	return NULL;
}

//only works inside template too
bool Server::OnTryUseUseItemAll(Player* player, int itemId, Vector3 dir, int objectId, MethodInfo* method)
{
	Server::EmitState();
	if (!player) return true;

	return true;
}