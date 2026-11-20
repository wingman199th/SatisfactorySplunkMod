# Satisfactory Splunk Exporter

A Satisfactory (SML) mod that streams live factory telemetry to Splunk via the
HTTP Event Collector (HEC):

| Sourcetype | Interval (default) | What you get |
|---|---|---|
| `satisfactory:status` | at startup | One event when the exporter starts — proves the connection works |
| `satisfactory:summary` | 5s | Machine/player/vehicle counts, idle machines, total power produced/consumed/capacity, battery charge, tripped fuses, average productivity |
| `satisfactory:power` | 5s | One event per power circuit: produced, consumed, capacity, headroom, battery charge/input, fuse tripped |
| `satisfactory:production` | 30s | One event per manufacturer/extractor/generator: recipe, items/min in and out (incl. clock speed and Somersloop boost; fluids in m³), buffered input/output, productivity, power draw, and a `status` field (`producing`, `waiting_for_input` + which items, `output_backed_up`, `no_power`, `waiting_for_fuel`, `paused`, `no_recipe`) |
| `satisfactory:inventory` | 30s | Total count of every item type across all storage containers |
| `satisfactory:vehicle` | 10s | Trucks (position, speed, fuel, cargo), trains (position, speed, next station), freight wagon cargo |
| `satisfactory:player` | 10s | Player name, online flag, position, speed, in-vehicle flag |

All intervals and data types are configurable in
`FactoryGame/Configs/SatisfactorySplunkMod.ini` (created on first launch).
Events are batched and POSTed to HEC on a flush timer (default every 5s).

## Compatibility

Verified against the SatisfactoryModLoader headers updated **2026-11-04**
(game build CL517554, SML 3.12.0, engine **UE 5.6.1-CSS**). Every game API
the mod calls was checked to exist in those headers.

A C++ mod binary only loads in the game build it was compiled against, so
**after a game patch, update your SatisfactoryModLoader checkout (and the
engine version it names in `FactoryGame.uproject`) and rebuild**. A build
made against an older engine/game version will not load.

## History

- **2.1** — first version expected to work end to end. Fixes the reasons
  2.0 never ran in-game: the `Config` folder was never packaged (no
  `Config/PluginSettings.ini`), so the exporter never found its settings;
  config now lives in `FactoryGame/Configs/` where SMM updates can't wipe
  it, and is auto-created on first launch. Also: correct `RequiredOnRemote`
  descriptor key, no duplicate export from multiplayer clients, accurate
  circuit collection, Somersloop boost and fluid units in throughput,
  immediate startup event, HTTP request chunking, rate-limited error logs.
- **2.0** — rewrite of 1.x, which called FactoryGame APIs that don't exist
  and could never compile.

## Building the mod

There is no prebuilt release, so you build it with the standard
Satisfactory modding toolchain. Follow the official SML development setup
guide at **https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/index.html** — in short:

1. Install Visual Studio 2022 (with C++ game development workload) and Wwise
   as described in the guide.
2. Clone (or `git pull` if you already have it) the modding project:
   ```
   git clone https://github.com/satisfactorymodding/SatisfactoryModLoader.git
   ```
3. Install the custom engine version named in `FactoryGame.uproject`
   (currently `5.6.1-CSS`) from the link in the guide.
4. Copy **this repository** into the modding project as
   `SatisfactoryModLoader/Mods/SatisfactorySplunkMod/`
   (the folder containing `SatisfactorySplunkMod.uplugin`).
5. Right-click `FactoryGame.uproject` → *Generate Visual Studio project
   files*, open the solution, build the *Development Editor* target.
6. Open the project in the editor, open **Alpakit** (Window → Alpakit), select
   `SatisfactorySplunkMod`, and click *Alpakit!* to package it.

Then follow **[INSTALL.md](INSTALL.md)** to install, configure and verify.

## Splunk setup

1. In Splunk: **Settings → Data Inputs → HTTP Event Collector → New Token**.
   Name it (e.g. `satisfactory`), pick or create an events index
   (e.g. `satisfactory`), and copy the token.
2. **Certificate gotcha (the most common reason "no data arrives"):** Splunk's
   default HEC certificate is self-signed and the game's HTTP stack will
   reject it. Either:
   - disable SSL on HEC (*HTTP Event Collector → Global Settings → uncheck
     Enable SSL*) and use `http://your-server:8088/services/collector/event`
     (fine on a LAN), or
   - put a certificate the OS trusts in front of HEC.
3. Test the endpoint from the machine that runs the game with the included
   script, which sends exactly what the mod sends:
   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\Test-SplunkHec.ps1 -Url http://your-splunk:8088/services/collector/event -Token YOUR-TOKEN
   ```
   It must print `HTTP 200` before the mod can work.

## Configuring the mod

On first launch the mod creates `<game>/FactoryGame/Configs/SatisfactorySplunkMod.ini`.
Set `SplunkURL` and `HECToken` there and restart the game. (This location
survives mod updates; the mod folder itself is replaced on every SMM update.)
On dedicated servers you can instead set the `SPLUNK_HEC_URL`,
`SPLUNK_HEC_TOKEN` and `SPLUNK_HEC_INDEX` environment variables, which
override the file.

Data is collected only on the host (single-player, multiplayer host, or
dedicated server). Clients joining don't need the mod
(`"RequiredOnRemote": false`), and clients that do have it stay idle.

Check `FactoryGame/Saved/Logs/FactoryGame.log` for lines starting with
`LogSatisfactorySplunkMod` to confirm it's collecting and sending.

## Example Splunk searches

```spl
# Live power: production vs consumption
index=satisfactory sourcetype="satisfactory:summary"
| timechart span=10s latest(event.power_produced_mw) as Produced latest(event.power_consumed_mw) as Consumed

# What is every stalled machine waiting for?
index=satisfactory sourcetype="satisfactory:production" event.status="waiting_for_input"
| stats latest(event.status) by event.machine_id, event.recipe
| rename event.* as *

# Actual output throughput per item (items/min)
index=satisfactory sourcetype="satisfactory:production"
| spath event.outputs{} output=outputs | mvexpand outputs | spath input=outputs
| timechart span=1m sum(per_min_actual) by item

# How much of everything is in storage
index=satisfactory sourcetype="satisfactory:inventory"
| timechart span=1m latest(event.count) by event.item

# Player positions (for a scatter/map panel)
index=satisfactory sourcetype="satisfactory:player"
| table _time event.player event.x event.y event.z event.speed_kmh

# Machines with tripped fuses / circuits in brownout
index=satisfactory sourcetype="satisfactory:power" event.fuse_triggered="true"
```

## Performance notes

- The production collector iterates every machine; on mega-factories keep
  `ProductionInterval` at 30–60s. Summary/power collectors are cheap.
- If Splunk is unreachable, events from failed sends are dropped and counted;
  errors are logged on the first failure and then about once a minute. The
  game is never blocked. Large batches are split into requests of 500 events.

## Project layout

```
SatisfactorySplunkMod.uplugin        plugin descriptor (SML ^3.12)
Config/SatisfactorySplunkMod.ini     config template (copied to FactoryGame/Configs/)
Config/PluginSettings.ini            makes packaging ship Config/ as loose files
Config/Alpakit.ini                   build targets (Windows, WindowsServer, LinuxServer)
tools/Test-SplunkHec.ps1             tests your Splunk HEC setup without the game
Source/SatisfactorySplunkMod/
  Public/SplunkExporter.h            exporter actor
  Private/SplunkExporter.cpp         collectors + HEC client
  Public/SplunkGameWorldModule.h     SML root module (auto-spawns exporter)
  Private/SplunkGameWorldModule.cpp
  Public/SatisfactorySplunkMod.h     UE module + log category
  Private/SatisfactorySplunkMod.cpp
```

## License

MIT
