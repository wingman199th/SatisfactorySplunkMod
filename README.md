# Satisfactory Splunk Exporter

A Satisfactory (SML) mod that streams live factory telemetry to Splunk via the
HTTP Event Collector (HEC):

| Sourcetype | Interval (default) | What you get |
|---|---|---|
| `satisfactory:summary` | 5s | Machine/player/vehicle counts, total power produced/consumed/capacity, average productivity |
| `satisfactory:power` | 5s | One event per power circuit: produced, consumed, capacity, battery input, fuse tripped |
| `satisfactory:production` | 30s | One event per manufacturer/extractor/generator: recipe, items/min in and out, productivity, clock speed, power draw, and a `status` field (`producing`, `waiting_for_input` + which items, `output_backed_up`, `no_power`, `waiting_for_fuel`, `paused`, `no_recipe`) |
| `satisfactory:inventory` | 30s | Total count of every item type across all storage containers |
| `satisfactory:vehicle` | 10s | Trucks (position, speed, fuel, cargo), trains (position, speed, next station), freight wagon cargo |
| `satisfactory:player` | 10s | Player name, position, speed, in-vehicle flag |

All intervals and data types are configurable in
`Config/SatisfactorySplunkMod.ini`. Events are batched and POSTed to HEC on a
flush timer (default every 5s).

## Version 2.0 — why the rewrite

Version 1 of this mod never compiled: it called FactoryGame APIs that don't
exist (`AFGBuildablePowerGenerator`, `GetPowerProduction()`,
`GetProductionEfficiency()`, instance `GetDisplayName()` on item descriptors,
and more), and its install instructions (copying source files into
`FactoryGame/Mods/`) can't work — SML C++ mods must be compiled inside the
modding SDK and packaged with Alpakit.

2.0 was rewritten against the real headers from the
[SatisfactoryModLoader](https://github.com/satisfactorymodding/SatisfactoryModLoader)
project (SML 3.12 / game 1.1) — every game API call in this mod was verified
to exist there. It also replaces the fragile "metrics index" mode with plain
JSON events, reads config deterministically from the mod's own ini file, and
adds the data people actually want on a dashboard: per-machine throughput,
what a machine is waiting for, and storage totals per item.

## Building the mod

There is no prebuilt release, so you build it once with the standard
Satisfactory modding toolchain. Follow the official SML development setup
guide at **https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/index.html** — in short:

1. Install Visual Studio 2022 (with C++ game development workload) and Wwise
   as described in the guide.
2. Clone the modding project:
   ```
   git clone https://github.com/satisfactorymodding/SatisfactoryModLoader.git
   ```
3. Install the custom engine (Unreal Engine – CSS) linked from the guide
   (requires joining the Epic Games organization as described there).
4. Copy **this repository** into the modding project as
   `SatisfactoryModLoader/Mods/SatisfactorySplunkMod/`
   (the folder containing `SatisfactorySplunkMod.uplugin`).
5. Right-click `FactoryGame.uproject` → *Generate Visual Studio project
   files*, open the solution, build the *Development Editor* target.
6. Open the project in the editor, open **Alpakit** (Window → Alpakit), select
   `SatisfactorySplunkMod`, and click *Alpakit!* to package and copy it into
   your game's `FactoryGame/Mods/` folder (or package a zip for SMM).

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
3. Test the endpoint from the machine that runs the game:
   ```
   curl http://your-splunk:8088/services/collector/event \
     -H "Authorization: Splunk YOUR-TOKEN" \
     -d '{"event":"test"}'
   ```
   You must get `{"text":"Success","code":0}` before the mod will work.

## Configuring the mod

Edit `Config/SatisfactorySplunkMod.ini` **inside the installed mod folder**
(`<game>/FactoryGame/Mods/SatisfactorySplunkMod/Config/`), set `SplunkURL`
and `HECToken`, and restart the game. On dedicated servers you can instead
set the `SPLUNK_HEC_URL` and `SPLUNK_HEC_TOKEN` environment variables, which
override the ini.

Data is collected on the host (single-player, multiplayer host, or dedicated
server) — clients joining a server don't need the mod for it to export
(`AcceptsAnyRemoteVersion` is set).

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
- If Splunk is unreachable the mod buffers up to `MaxBufferedEvents` events
  (oldest dropped first) and logs an error — the game is never blocked.

## Project layout

```
SatisfactorySplunkMod.uplugin        plugin descriptor (SML ^3.12)
Config/SatisfactorySplunkMod.ini     runtime configuration
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
