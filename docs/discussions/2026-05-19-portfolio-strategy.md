# Portfolio Strategy Discussion — 2026-05-19

> **Superseded by 2026-05-21 scope decision:** This repo (`iouring-net-lib`) now ships **chat server only** as its complete portfolio deliverable. Layer 3 (MMORTS / primitive MMO / renderer thread) has moved to a **separate future repository** (name TBD). The three-layer plan below is preserved as historical context — it explains the decision lineage, but the v1 ship plan is chat-only. See `.omc/wiki/chat-server-v1-session-and-auth-design.md` and `project-portfolio-scope` memory for the current scope.

Companion record to `2026-05-19-server-architecture.md`. Captures the portfolio
positioning, the three-layer plan, the genre decision for the portfolio game,
and the reasoning behind each step.

---

## TL;DR

**The plan**: three layers, each shippable individually, each building on the last.

1. **Network library** (`iouring-net-lib`) — the systems portfolio piece. Already designed; needs implementation.
2. **Chat server** — proves the framework handles messaging. Recognizable demo.
3. **Faction-based squad-commander MMORTS** — proves the framework handles game-style real-time simulation. Genre: 2D top-down where you play a centurion/captain controlling 5–10 autonomous AI soldiers, in a faction at persistent war with one or more other factions. (Evolved from the earlier "squad-commander mock MMO" framing — see Part on faction-based design below.)

**Separately**: a strategy game (RTS/TBS) built in UE5 or Unity is its own portfolio track for gameplay/design skills, NOT for showcasing the network library.

The dual-portfolio (custom systems + commercial engine) is stronger than either alone — it demonstrates tool-selection judgment, not just stubbornness.

---

## Three-layer portfolio plan

### Layer 1: Network library

Already designed in `wiki/` and `docs/`. The architecture decisions captured in `2026-05-19-server-architecture.md`. Implementation hasn't started in earnest — `lnx::atomic*`, `lnx::mutex`, `lnx::thread`, and `sds::ring_buffer` have landed; the network primitives (`recv_ring_buffer`, `MemoryPool`, `io_uring_reactor`, `Session`, etc.) are spec-only.

This is the **systems portfolio**: low-level C++20 + io_uring + custom memory management + design-first methodology. Target audience: server/infrastructure/engine teams that hire on systems engineering.

### Layer 2: Chat server

The chat server proves the network library works for messaging. The architectural elements it exercises:

- Channel-per-thread model → maps directly to "chat rooms"
- Login + auth + session routing → user joins, gateway routes to room
- Within-channel broadcast → message in room goes to every member
- Cross-channel POD messaging → server-wide announcements, DMs across rooms
- TLS `ObjectPool` → message objects, session objects, room membership
- io_uring kernel boundary → all socket I/O proves the API choices
- Real-time semantics → message delivery latency is observable

### Layer 3: Squad-commander mock MMO

Decided 2026-05-19 after extensive genre discussion. Game shape: 2D top-down, you play one character (a centurion/captain) who commands 5–10 AI soldiers in a small unit. World has multiple battlefield zones (= channels = content threads). Other players are other commanders coordinating tactically.

This layer proves the framework handles game-style tick-based simulation with entity AI, real-time movement, combat, AOI, channel routing — beyond what chat exercises.

---

## Why the chat server works as a portfolio piece

Chat alone risks being categorized as "tutorial-grade." The framing matters:

### Weak positioning

> *"I built a chat server in C++."*

Reviewers skim, categorize as tutorial, move on.

### Strong positioning

> *"A Linux-native real-time service framework on io_uring and C++20 coroutines, demonstrated by a high-throughput chat server."*

Now chat is the *demo*, not the *product*. Reviewers go looking for the framework, find the architecture writeups, and realize the chat is showcasing real systems work.

### What makes "portfolio chat" vs "tutorial chat"

The chat server itself can be modest. The portfolio strength is in what surrounds it:

1. **Documented architecture decisions.** The design docs in `wiki/` and `docs/` ARE the portfolio. Most candidates can't articulate this stuff at this level. Make sure the writing shows up prominently.
2. **Real benchmarks.** "N connections per core at p99 < X ms, messages-per-second per channel = Y, scales linearly to M channels across M cores." Numbers turn theory into evidence.
3. **Specifically non-trivial features** that tutorials skip:
   - Graceful shutdown with full drain (the lifecycle invariant)
   - Reconnect with session state recovery
   - Presence (who's online) with no race conditions across channels
   - Cross-channel DMs (proves POD message channel works)
   - Rate limiting per connection
   - Observability — structured logs, metrics
4. **Honest tradeoff writing.** "I chose io_uring over epoll because X. I chose single-thread-per-channel over multi-threaded content because Y." The explanation itself signals expertise.

---

## Game genre decision

### Why io-games (agar.io / slither.io style) were initially recommended

For a network engine portfolio, the genre should make visible **to the audience watching the demo**:
- Many players in one channel (proves AOI, broadcast, single-core ceiling)
- Real-time movement (smooth vs janky = immediate quality signal)
- Multiple parallel channels (proves channel-per-thread scales)
- Clean lobby → game transition

io-games tick all these boxes with minimal gameplay scope.

### Why MMORPG turned out to be a stronger choice

Stephen's pushback: **io-game bots need to play competitively to stress the engine realistically.** Random-walking bots don't push the engine the way real players do. To make a meaningful load test, you'd be writing actual competitive game AI — its own multi-week project.

NPC AI in MMORPG is the opposite — *"dumb monsters" are a feature, not a bug.* Wander aimlessly → aggro if player approaches → attack → return to spawn. A 4-state FSM, half a day of work, and it's exactly what Lineage/MapleStory/Diablo monsters do.

Comparison:

| Aspect | io-game | Mock MMORPG |
|---|---|---|
| Demo with one connected player | Nothing happens | Monsters wandering, world feels alive |
| Bot AI complexity | High (competitive) | Trivial (wander + aggro FSM) |
| Architecture exercised | Channels, AOI, broadcast | Same, plus PvE entity lifecycle |
| Players per channel | 30–50 | 10–20 + 50–100 monsters = same entity load |
| Genre recognition | "Some io game" | "An MMORPG" — instantly understood |
| Korean game industry relevance | Medium | **High** — direct lineage |

### Why MMORTS (Stephen's preferred genre) is genuinely hell for bot AI

Stephen's stated preference: strategy games (RTS like Total War/StarCraft, TBS like Civ). MMORTS specifically, "you become a company commander / centurion."

The problem is bot AI complexity. For traditional RTS:
- Manage many units (coordinate movement, formation, targeting): hard
- Strategic decisions (build order, expansion, tech): very hard
- Tactical decisions (when to engage, retreat, flank): hard
- Resource management: hard

Even bad RTS AI is much harder than bad action-game AI. LoL's "intro bots" are simple and feel terrible. Real RTS bots are research projects.

For commercial-engine strategy games (UE5/Unity), this complexity is acceptable — different portfolio.
For a custom network engine portfolio, the bot complexity dominates the work and doesn't showcase the engine.

### The unlock: "commander of autonomous units" hybrid

The breakthrough: **if unit AI lives server-side (autonomous), and players just give orders, the bot complexity drops to MMORPG-level while preserving the MMORTS aesthetic.**

```
Traditional RTS:                  Commander-style:
  Player controls 200 units         Player controls 1 hero
   directly                          + commands 5-10 AI soldiers

  Bot AI = full RTS brain           Bot AI = "send random
   (target selection, formation,      attack order to nearest
    flanking, resource management)    enemy unit every N seconds"

  ~ months of AI work               ~ 30 lines
```

This collapses bot complexity to roughly MMORPG-level, while preserving the "company leader" feel because:

- **Unit AI lives server-side** — same as MMORPG monsters (wander, aggro, attack FSM)
- **Player just gives orders** — "attack target X", "hold position", "follow me"
- **Bot client just sends random orders** — trivial
- **Strategic gameplay** comes from coordinating with other real human commanders, not from per-unit micro

Real games in this space:
- **Foxhole** — each player is one soldier in a massive war zone, with commander coordination
- **Mount & Blade Bannerlord** — captain mode: each player leads a unit of ~10 AI soldiers
- **Hell Let Loose / Squad** — squad leaders give orders, soldiers execute
- **Classic browser MMOs (Travian, Tribal Wars)** — give orders, units act autonomously

Foxhole is the closest mainstream example. Architecturally it really is "MMORPG with weapons and unit AI."

---

## Final game target: Squad-commander mock MMO

### Concrete shape

- **2D top-down view** — Ultima Online / classic Diablo / RuneScape Classic aesthetic. No 3D, no animation rigs.
- **You play a centurion/captain** — one player character on the map, named, recognizable.
- **Your squad is 5–10 AI soldiers** — different unit types (spearmen, archers, shieldmen, cavalry). Follow you in formation. You change formation, issue orders.
- **Server runs all unit AI** — wander when idle, formation-march when following, engage when in combat. Same FSM pattern as MMORPG monsters, attached to player commanders instead of spawn points.
- **Orders are the player input** — "form line", "attack target", "hold position", "charge that hill". Discrete actions, not continuous micromanagement. Server resolves consequences.
- **World has battlefield zones** — each zone is a content thread. Different terrain, objectives.
- **Multiple commanders coordinate** — chat server doubles as strategy layer (call out enemies, plan attacks). MMORTS feel comes from real human coordination.
- **Enemy squads** — server spawns NPC enemy formations that wander/patrol/engage. PvE first, PvP later.
- **Bot clients are trivial** — pick a random soldier-target in view, send "attack" order. Or just follow another player.

### Faction-based extension (2026-05-19 evening)

The squad-commander design evolved further during the same discussion into a **faction-based MMORTS** — the version that's actually fun to play and demo:

- **2 or more factions** in persistent war. (3 factions is strategically richer — PlanetSide-style emergent alliances, betrayals, kingmaker dynamics. 2 factions is simpler — Foxhole-style.)
- **Each faction has multiple players** — squad commanders coordinating tactically against the other faction(s).
- **Bot clients are trivial because of faction comparison** — `bot.faction == target.faction` decides friend/foe, "nearest enemy → attack" decides target. ~30 lines for a useful load-test bot.
- **Persistent war state** — territory, resources, leadership shared across all channels via global singletons.

Why faction-based is the unlock vs single-faction PvE:

- **Emergent multiplayer dynamics from simple rules** — cooperation within faction, competition across factions. Real conflict, not NPC contrivance.
- **Bots fill out the population** — assigned to factions at login, behave normally as commanders. World feels alive without writing strategic AI.
- **Engine demo becomes a real game** — not "tech demo with simulated opponents" but "small persistent multiplayer war." Same architectural cost; much stronger portfolio narrative.

Industry precedents (closest in design space):

- **Foxhole** — 2 factions (Warden vs Colonial) in persistent war. Closest mainstream example.
- **PlanetSide 1 & 2** — 3 factions; gold standard for emergent strategy (alliances, betrayals, kingmaker).
- **Hell Let Loose / Squad** — faction-based squad-leader-commands-soldiers.
- **Classic browser MMOs (Travian, Tribal Wars)** — faction persistence with give-orders-to-autonomous-units.

Architectural fit (every faction mechanic maps cleanly to the bifurcation in the architecture doc):

| Element | Category | Primitive |
|---|---|---|
| Faction state (territory, resources, leadership) | Global shared mutable | `FactionState` singleton per faction + `lnx::mutex` (writers); content threads read via RCU snapshot |
| Battle/zone state | Channel-local | TLS pools per battlefield channel |
| Faction-wide radio chat | Cross-channel POD broadcast | Broadcaster fans out POD to all channels with members of that faction |
| Battle outcome → faction territory | Cross-channel POD event | Battle channel sends "won zone X" POD to faction-state owner thread |
| Player faction assignment | Set at login, stored on Session | Immutable per session |

No new primitives needed beyond what the network library already designs. The faction-based extension is purely a **content layer** addition; the network engine doesn't change.

### Open design questions for the faction-based variant

1. **2 factions vs 3** — PlanetSide-style 3-faction has demonstrably richer meta; 2-faction is simpler to balance. Worth deciding before art/lore work begins.
2. **Persistence model** — Foxhole-style "war ends after weeks, world resets" or PlanetSide-style "continent capture rotates"? Affects state schema and channel teardown semantics.
3. **Inter-channel coordination mechanism** — voice chat? text? unit-ping system across the strategic map? Affects how the chat server's role evolves.
4. **Aesthetic / setting** — Roman/medieval (matches "centurion" framing)? Sci-fi (PlanetSide-like)? WW2 (Hell Let Loose-like)?

### Why this works

1. **Architecture demonstration**: identical to mock MMORPG — channel-per-thread, TLS pool for entities and sessions, in-channel broadcast for combat events, cross-channel POD for zone transitions and whispers.
2. **Passion alignment**: Stephen genuinely likes strategy games. Passion is the prerequisite for finishing.
3. **Bot simplicity**: ~30 lines of "send random orders" creates real network load.
4. **Recognizability**: Foxhole-like, Mount & Blade Captain Mode-like. Reviewers immediately understand.
5. **Achievability**: Comparable scope to mock MMORPG, maybe slightly more on server side (unit AI) but offset by simpler gameplay UI.
6. **Visual demo**: Units moving in formation looks impressive without requiring AAA art.

---

## Industry context

### Korean game industry

Stephen is at i-screammedia in Korea. Likely portfolio audience includes Korean game industry (Nexon, NCSoft, Krafton, Smilegate, Pearl Abyss). For that audience:

- **Custom C++ network engine + low-level systems** is highly valued and matches their hiring tradition
- **MMORPG-style server architecture** is the dominant genre in their portfolio
- **Korean studios have a deep MMO tradition** — Lineage, MapleStory, Black Desert, Lost Ark, AION — and value engineers who understand zone-server architecture
- **Squad-commander mock MMO** directly maps to the kinds of systems they build

### International gaming/realtime backend

Same package also works for Discord platform team, Riot's networking team, Roblox engine team, Cloudflare, Fastly:
- Framework story is universal
- Chat proves messaging
- Game proves real-time gameplay
- Dummy-client load tests prove performance

### The dual-portfolio strength

| Custom (net lib + client) | Commercial (UE5 / Unity) |
|---|---|
| Low-level C++ systems | Rendering, animation, audio, content pipelines |
| io_uring / network architecture | Built-in replication systems, lobby SDKs |
| Memory management, TLS pools, lock-free design | Asset workflow, level design, balancing iteration |
| Deterministic + measurable performance | Gameplay polish, visual fidelity |
| Audience: server/infra/engine teams | Audience: gameplay designers, gameplay engineers |

These are two different jobs in most studios. Server engineer and gameplay engineer are usually separate hiring tracks (especially in Korean studios). The right tool depends on which job you're proving you can do.

A portfolio that's all-custom signals "I do everything from scratch even when inefficient" — sometimes read as inability to choose tools.

A portfolio that's all-commercial signals "I drive engines but don't understand what's underneath" — sometimes read as shallow.

A portfolio that's *clearly split by judgment* signals **the engineer who picks the right tool for each problem.** That's what senior hiring is actually looking for.

---

## The passion principle

**Stephen's observation:** *"game portfolio needs passion. Without passion you do not have to work in game industry lol."*

This is right and worth stating explicitly. Game industry realities:

- Long hours, lower pay than other tech sectors
- Projects get canceled
- Layoffs are common
- Crunch culture (though improving)

The only thing that makes it worth it is genuinely loving games and game-making. If you're doing it for money/prestige, you'll burn out or get cynical fast.

**Portfolio implication**: A genre you love and ship beats an "optimal" genre you abandon. Personal motivation determines completion, completion determines polish, polish determines portfolio strength.

The squad-commander hybrid was chosen because it threads the needle: it's a strategy-flavored game (passion-aligned) that fits the network engine (architecturally correct). Both axes align.

---

## Implementation considerations

### What the custom client needs to be

For demonstrating the network engine:
- 2D OpenGL (or SDL2 + minimal renderer) is sufficient
- DirectX is not specifically required despite earlier mentions
- The graphics quality isn't the portfolio piece; **the network engine's visible behavior through the graphics** is
- Just enough rendering to make packet flow and state sync visible to a viewer
- Don't burn engine-portfolio time building a renderer that won't be remembered — spend that time on benchmarks, load-testing harness, and writeups

### Headless dummy client

Critical and underweighted in most portfolios:
- Stress-tests the engine at scale
- Proves the engine handles real load (not just demoable load)
- Is itself a portfolio piece — shows you can write reliable test infrastructure
- Bot AI: "send random valid orders at realistic intervals" — server load is the same as smart bots
- Should support spawning N bots from one process for load-testing efficiency

### Order of operations

1. **Fix the `lnx::mutex` / `lnx::shared_mutex` release-mode bug** (see architecture doc Part 9)
2. **Build smallest end-to-end thing** — one channel, one session, echo. ~500–1000 lines. Validates the architecture transfers to code.
3. **Iterate to v1** — `recv_ring_buffer` / `send_ring_buffer` specializations, `MemoryPool`, `Session`, packet framing
4. **Chat server on top of v1** — proves messaging
5. **Squad-commander game on top of chat** — proves real-time gameplay
6. **Polish + benchmarks + writeups** — what makes it a portfolio piece
7. (Separately) commercial-engine strategy game for design portfolio track

---

## Scaling reality check

Stephen identified the architecture's known ceiling: *"limit of user experience in this conventional real time server architecture is dependent on what one CPU core can do then, multi threading has its own limit for over 10000 people in one content thread."*

This is correct and is the **designed-in limit** of single-thread-per-channel:

- Realistic per-channel capacity: 300–3,300 players (depending on per-player cost)
- Production game numbers: 10 (MOBA) to 200 (MMO zone) to 5,000 (EVE with time dilation)
- Past ~5,000 in one interaction context: distributed simulation territory (Star Citizen Server Meshing, SpatialOS), fundamentally different problem

For the squad-commander mock MMO, comfortably within ceiling: 20 commanders × 7 soldiers = 140 entities per channel, well below the limit even with collision/combat logic.

The architecture handles the design space the portfolio game targets without needing escape hatches.

---

## Summary of decisions made

1. **Three-layer portfolio plan**: network library → chat server → mini game
2. **Mini game genre**: 2D top-down squad-commander mock MMO ("commander of autonomous units")
3. **Bot client architecture**: independent client process, not server-side actor. Bots are just dumb clients sending random valid packets.
4. **Custom client renderer**: 2D OpenGL or SDL2, minimal — just enough to visualize network state
5. **Separate UE5/Unity strategy game**: a different portfolio track for design/gameplay skills, NOT for showcasing the network library
6. **Dual-portfolio positioning**: deliberate tool selection signal
7. **Order of operations**: fix mutex bug → smallest end-to-end → v1 → chat → game → polish

---

## What this discussion did NOT cover but might be worth following up on

- Specific tech stack choice for the custom client (SDL2 vs raw GLFW + OpenGL vs other)
- Server-client protocol design (beyond the 4-byte header in the spec)
- Persistence layer for the squad-commander game (saving player/squad state)
- Matchmaking algorithm for routing to battlefield zones
- How "commander coordination" actually works in-game (voice chat? text? unit ping system?)
- Anti-cheat considerations
- Concrete benchmark plan and tooling for the load tests
- Deployment story (single binary? containers? bare metal? cloud?)

These are all "later" concerns — none block the next concrete step (fix the mutex bug, build the smallest end-to-end thing).
