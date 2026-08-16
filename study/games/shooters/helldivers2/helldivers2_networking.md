# Helldivers 2 — three networks, not one

How Helldivers 2 is networked, read from the retail install. Parent note:
[`helldivers2.md`](helldivers2.md).

Almost every public discussion of this game's netcode collapses three separate
systems into one word, "servers", and then argues about it. The install
separates them cleanly, and they are built from different technology, by
different vendors, with different failure modes:

| Tier | What it carries | Technology | Evidence |
|---|---|---|---|
| **Mission** | the 4-player simulation | **PlayFab Party** relay mesh + Bitsquid object replication | `PartyWin.dll` exports |
| **Session** | lobbies, matchmaking, invites, ownership | **PlayFab Multiplayer** | `PlayFabMultiplayerWin.dll` exports |
| **Galactic War** | progression, war state, unlocks | **REST over libcurl** | `libcurl.dll`, WinHTTP, WININET |

When people say "the servers are down" they are usually describing tier 3 or
tier 2; when they say "the netcode is bad" they are usually describing tier 1's
host-authority model. The three fail independently and this note treats them
separately.

Tags as [`helldivers2.md`](helldivers2.md).

---

## 1. Tier 1: PlayFab Party carries the mission  [BUILD]

`bin/PartyWin.dll` is Microsoft's **PlayFab Party** — and critically, it is
present with its **data plane**, not just its voice API. The exported surface
includes both halves:

```
# data plane
PartyNetworkCreateEndpoint      PartyNetworkDestroyEndpoint
PartyEndpointSendMessage        PartyEndpointFlushMessages
PartyEndpointCancelMessages     PartyEndpointGetEndpointStatistics
PartyNetworkGetDevices          PartyNetworkGetEndpoints
PartyNetworkGetDeviceConnectionType
PartyNetworkGetNetworkStatistics   PartyNetworkGetNetworkDescriptor
PartyNetworkKickDevice          PartyNetworkKickUser
PartyNetworkCreateInvitation    PartyNetworkRevokeInvitation
PartyNetworkAuthenticateLocalUser

# voice
PartyChatControlSetAudioInput   PartyChatControlSendText
PartyChatControlSetAudioEncoderBitrate
PartyChatControlSetTranscriptionOptions
PartyChatControlGetAvailableTextToSpeechProfiles
```

**The presence of `PartyEndpointSendMessage` is what settles the question.**
Party can be adopted for voice chat alone; this build uses it as the **game
data transport**. Squad voice and squad state ride the same network object.

What Party actually is matters for how the game behaves. It is not raw
peer-to-peer with NAT traversal — it is a **relay mesh**: devices authenticate,
join a *Party network* hosted on relay infrastructure, and create logical
*endpoints* that send messages to each other through it.
`PartyNetworkGetDeviceConnectionType` exists precisely because a device may be
directly connected or relayed.

**[inferred]** Three consequences fall out, and each maps to a behaviour
players report:

* **Connectivity is very good; latency has a floor.** A relay means no NAT
  punch-through failures — which is why Helldivers 2 squads form across
  networks that defeat classic P2P — and it also means every packet takes a
  detour through a relay region. You trade tail-latency reliability for a
  latency floor, which is the correct trade for a co-op PvE game and the wrong
  one for a shooter with duels.
* **Kicking is a transport operation.** `PartyNetworkKickDevice` /
  `PartyNetworkKickUser` are relay-enforced, not gameplay-enforced.
* **The mission network has an owner-ish shape but no dedicated server.**
  Party gives a mesh of equal endpoints. Somebody still has to be authoritative
  over the simulation, and Party does not provide that. §2 does.

---

## 2. Underneath Party: Bitsquid object replication  [BUILD] [ENGINE]

Party moves bytes. What decides *which* bytes is the engine's own network layer,
and its error vocabulary survives in the shipped plugin foundation — these
strings are in `level_generation_pluginw64_release.dll`, which links the same
Stingray SDK the game plugin does:

```
ReliableSendQueueOverflow      ReliableSendBufferOverflow
ReliableReceiveBufferOverflow  TransmitOverflow
PongTimeout                    DestroyPlatformSession
TransportSendError             TransportConnectionError
Platform_AuthenticatorDenied   Platform_PongTimeout
Platform_TransportConnectionError   Platform_TransportSendError

Invalid  Creating  Joining  Joined  Failed  Close     ← session states
Default  Worldwide                                    ← matchmaking scope
```

Two layers are visible in that list. `Reliable*` and `Transmit*` are the
engine's own reliable-stream bookkeeping. The `Platform_`-prefixed duplicates
are a **platform transport abstraction** — the seam where Stingray's networking
meets PlayFab Party on PC and whatever Sony's equivalent is on PS5. The `Pong`
in `PongTimeout` is a ping/pong liveness check owned by the engine, not by
Party.

Frykholm documented the layer these strings belong to **[ENGINE]**:

> The entire network model is based on an packet delivery system (on top of
> UDP) that provides ACKs for unreliable packets as well as a reliable (and
> ordered) packet stream between any two network endpoints. At the next layer
> we have implemented a remote-procedure-call service for Lua as well as an
> object replication system.

and stated the design preference plainly:

> Games can use these services however they like, but our recommendation is to
> do as much as possible with the object replication system and as little as
> possible with RPC calls, since using explicit RPC messages tends to require
> more bandwidth and be more error prone.

The replication protocol is a create/update/destroy stream where creation and
destruction are reliable and updates are not:

```
A: CREATE [wait for ack] UPDATE_1 UPDATE_2 ... UPDATE_n DESTROY
```

> The *UPDATE* messages are sent on the unreliable stream (for maximum
> performance), so they can potentially arrive before *CREATE* or after
> *DELETE*. But this is not a problem, because we simply ignore *UPDATE*
> messages that arrive out of order.

**[inferred]** The three `Reliable*Overflow` strings are therefore the failure
mode that matters: the reliable stream is a **bounded queue**, and a burst of
creates and destroys can overrun it. A Helldivers 2 mission is a machine for
generating exactly that burst — a bug breach spawning forty entities, an
orbital laser destroying thirty, a Hellbomb doing both in one frame. Whatever
the observed symptom of a stressed session is, `ReliableSendQueueOverflow` is
the shape of the underlying problem, and it is a *capacity* failure rather than
a bandwidth one.

### 2.1 Host authority and migration, as designed by the engine

Frykholm's post is specifically about ownership migration in a peer-to-peer
session, and it describes Helldivers 2's model exactly **[ENGINE]**:

> The network can be run in both client-server and peer-to-peer mode. […]
> Migrating a network object means changing the owner of the object from one
> peer to another. There are a number of reasons why you might want to do that.
> First, if a player drops out of the game, the objects owned by that player
> may need to be taken over by somebody else.

and, importantly, who is allowed to do it:

> In our network, migration is implemented with a reliable *MIGRATION* message
> that tells everybody in the session about the object's new owner. The
> migration message is always sent by a special peer, the *HOST* of the game
> session. (To ensure that peers do not compete for the ownership of an
> object.)

So: a **HOST peer**, object ownership that can move, and migration mediated by
the host. That is Helldivers 2's squad-leader-hosts model, and it is the
engine's, not something Arrowhead invented.

The post then describes a race condition in it, which is worth reading in a
study of a game whose host migration is its most-complained-about system
**[ENGINE]**:

> The problem is that while the message system provides an ordered stream of
> messages between any two endpoints, there is no ordering of messages between
> *different* endpoints.

leading to a peer seeing `M_ab Ub Ub Ub D C Ua Ua Ua` — a migration before the
create, and a create after the delete.

> To be sure, this only happens if the object gets migrated *really* close to
> being created or deleted and if there are asymmetric network delays on top of
> that. But of course, it always happens to *someone*.

**[inferred]** This is not a claim that Helldivers 2 ships that specific bug —
the post is from 2013 and describes fixing it. It is a claim about the *class*
of problem the architecture has: **cross-endpoint ordering is not guaranteed,
and migration is the operation that exposes it.** A game that migrates
ownership when the host leaves, in a session where entities are created and
destroyed by the dozen per second, is operating in the exact regime the post
identifies as fragile — and "the remaining squad loses the session rather than
transferring smoothly" is what that fragility looks like from a sofa.

---

## 3. Tier 2: PlayFab Multiplayer runs sessions  [BUILD]

`bin/PlayFabMultiplayerWin.dll` exports the lobby and matchmaking SDK:

```
PFMultiplayerCreateAndJoinLobby   PFMultiplayerFindLobbies
PFMultiplayerConnectToLobby       PFLobbyAddMember   PFLobbyLeave
PFLobbySendInvite                 PFLobbyGetConnectionString
PFLobbyGetMembershipLock          PFLobbyForceRemoveMember
PFLobbyPostUpdate                 PFLobbyGetSearchProperty
PFLobbyGetOwner                   PFLobbyGetOwnerMigrationPolicy
PFMultiplayerCreateMatchmakingTicket   PFMatchmakingTicketGetMatch
PFMatchmakingTicketGetStatus      PFMatchmakingTicketCancel
PFMultiplayerCreateServerBackfillTicket
PFMultiplayerCreateAndClaimServerLobby
```

**`PFLobbyGetOwnerMigrationPolicy` is the one to notice.** Lobby ownership
migration is a *declared policy* on the lobby, handled by PlayFab's service —
and it is a completely different mechanism from the in-mission object
ownership migration of §2.1. **[inferred]** Losing the lobby owner and losing
the simulation host are two different events with two different recovery paths,
which is a plausible source of the compound failures players describe: the
lobby can survive while the mission does not, or vice versa.

The `*ServerLobby*` and `ServerBackfill` exports exist in the SDK and prove
nothing about use — they ship with the library whether or not a dedicated
server ever claims a lobby. Recorded here so a future reader does not mistake
their presence for evidence of dedicated servers. **The mission-tier evidence
in §1 is that the simulation runs on a peer, and nothing in this DLL
contradicts it.**

Identity on PC is Steam: `steam_api64.dll`, anchored at
`SteamInternal_FindOrCreateUserInterface`. The inventory contains one
`network_config` resource, named `generated` — consistent with the rest of
`data/game/`, where the tuning data is produced by a build step and encrypted.

---

## 4. Tier 3: the Galactic War is a REST client  [BUILD]

`libcurl.dll` ships and is imported, alongside `WINHTTP.dll`, `WININET.dll`,
`WS2_32.dll` and `IPHLPAPI.DLL`. There is no game-specific protocol library.

**[inferred]** The Galactic War — planet liberation percentages, Major Orders,
warbond progression, ship modules — is an HTTP API polled and posted to by the
client. That is the correct architecture for it: the war state changes on a
scale of hours, is identical for every player, and is authoritative on the
server. It also explains the failure signature everyone recognised at launch,
where missions were perfectly playable while the war map refused to load — a
tier-3 outage does not touch tier 1.

`data/game/generated_planet_data.dl_bin` (482 KB),
`generated_galactic_presence_settings.dl_bin` (142 KB) and
`generated_planet_territory_graphs.dl_bin` (1.5 MB) are the *static* half of the
war: the planets, their adjacency, their regions. The dynamic half — who owns
what right now — comes down the wire. **[inferred]** Splitting it that way keeps
the payload small: the client already has the galaxy's shape and only needs its
state.

`crs-client.dll` / `crs-handler.exe` are a crash reporting service, a fourth
outbound channel, and not part of gameplay.

---

## 5. Reading the whole stack at once

```
        Galactic War  ─── libcurl / HTTPS ──────► Arrowhead backend
                                                   (war state, progression)

        Lobby         ─── PlayFab Multiplayer ──► PlayFab service
                          PFLobby / PFMatchmaking  (matchmaking, invites,
                                                    owner migration policy)

        Mission       ─── Bitsquid object replication
                            └─ platform transport abstraction
                                 └─ PlayFab Party endpoints ──► relay mesh
                                     (+ voice on the same network)

        Authority     ─── one peer is HOST; owns most objects;
                          issues MIGRATION messages
```

**[inferred]** The design reads as a series of correct-for-the-genre choices
with one structural weakness. Buying Party removes an entire class of
connectivity failure and gets voice free. Buying PlayFab Multiplayer removes
matchmaking infrastructure. Keeping the engine's object replication means the
gameplay code never sees the transport. The weakness is that **the simulation
still lives on a consumer machine**, so a co-op PvE game with no competitive
integrity requirement inherits every host-departure problem that peer authority
has ever had — and inherits it in the exact regime (§2.1) where the engine's
own author documented the ordering guarantees breaking down.

---

## 6. What is worth taking

1. **Name your tiers.** Mission transport, session management and persistent
   backend are three systems with three vendors and three failure modes.
   Conflating them makes every outage unexplainable.
2. **A relay mesh is a real option, and its cost is a latency floor.** For
   co-op PvE that is the right trade; for anything with duels it is not. §1.
3. **Prefer object replication to RPC.** The engine's author recommends it, and
   the reason is bandwidth and error-proneness, not elegance. §2.
4. **The reliable stream is a bounded queue and bursts are your enemy.** Three
   distinct overflow error strings ship. Size for the spawn burst, not the
   average. §2.
5. **Ownership migration is where cross-endpoint ordering bites.** If you
   migrate, you need a rule for messages about an object arriving before its
   create and after its delete. §2.1.
6. **Split static world shape from dynamic world state.** The galaxy's
   adjacency ships on disc; who owns what comes down the wire. §4.
