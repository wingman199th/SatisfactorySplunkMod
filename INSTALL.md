# Installing the Satisfactory Splunk Exporter

This guide covers getting the **already-built** mod into the game and talking
to Splunk. If you still need to compile it, see the "Building the mod"
section in [README.md](README.md) first.

## Requirements

- Satisfactory **1.2** (the mod targets game version ≥491125, SML 3.12+)
- [Satisfactory Mod Manager (SMM)](https://smm.ficsit.app/) or a manual SML
  install — SML **3.12.0 or newer** must be present
- A Splunk instance with an HTTP Event Collector (HEC) token
- Clients joining your multiplayer game do **not** need the mod — it runs
  entirely on the host/server (`AcceptsAnyRemoteVersion` is set)

## Step 1 — Install the mod files

Pick whichever matches how you packaged it:

### Option A: Alpakit copied it for you (development install)

If you set your game path in Alpakit's settings and clicked **Alpakit!** with
"Copy to game" enabled, the mod is already at:

```
<game>\FactoryGame\Mods\SatisfactorySplunkMod\
```

Nothing more to copy — go to Step 2.

### Option B: Manual copy of the packaged mod

Alpakit's output zip (per platform, e.g. `Windows`) lands in the modding
project under `Saved\ArchivedPlugins\`. Extract it so the folder structure is:

```
<game>\FactoryGame\Mods\SatisfactorySplunkMod\
├── SatisfactorySplunkMod.uplugin
├── Binaries\
├── Content\ (Paks)
└── Config\
    └── SatisfactorySplunkMod.ini
```

Typical game locations:

- Steam: `C:\Program Files (x86)\Steam\steamapps\common\Satisfactory`
- Epic: `C:\Program Files\Epic Games\SatisfactoryEarlyAccess` (or `Satisfactory`)

### Option C: Dedicated server

Copy the same folder (use the **server platform** package from Alpakit —
`WindowsServer` or `LinuxServer`) to the server's `FactoryGame/Mods/`
directory. SML must be installed on the server too (SMM can manage servers,
or use [ficsit-cli](https://github.com/satisfactorymodding/ficsit-cli)).

## Step 2 — Set up Splunk HEC

1. In Splunk: **Settings → Data Inputs → HTTP Event Collector → New Token**.
   Pick or create an events index (e.g. `satisfactory`) and copy the token.
2. **Certificate warning — read this, it's the #1 failure cause:** Splunk's
   default HEC certificate is self-signed and the game's HTTP stack rejects
   it silently. On a home/LAN setup, go to *HTTP Event Collector → Global
   Settings* and **uncheck Enable SSL**, then use a plain `http://` URL.
   (Alternative: front HEC with a certificate your OS trusts.)
3. Verify from the machine that runs the game (PowerShell/cmd):

   ```
   curl http://your-splunk:8088/services/collector/event -H "Authorization: Splunk YOUR-TOKEN" -d "{\"event\":\"test\"}"
   ```

   You must see `{"text":"Success","code":0}` before continuing.

## Step 3 — Configure the mod

Edit (create the `Config` folder if your package didn't include it):

```
<game>\FactoryGame\Mods\SatisfactorySplunkMod\Config\SatisfactorySplunkMod.ini
```

Minimum required settings:

```ini
[Splunk]
SplunkURL=http://your-splunk:8088/services/collector/event
HECToken=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```

All collection intervals and per-type on/off switches are in the same file
with comments explaining each. Defaults are sensible for a mid-size factory;
on mega-factories raise `ProductionInterval` to 60.

**Dedicated servers:** instead of the ini you can set environment variables,
which override it — handy for containerized servers:

```
SPLUNK_HEC_URL=http://your-splunk:8088/services/collector/event
SPLUNK_HEC_TOKEN=xxxxxxxx-....
```

## Step 4 — Verify it works

1. Launch the game and **load a save** (data collection starts per-save).
2. Open `<game>\FactoryGame\Saved\Logs\FactoryGame.log` and search for
   `LogSatisfactorySplunkMod`. Healthy startup looks like:

   ```
   LogSatisfactorySplunkMod: SplunkGameWorldModule: SplunkExporter spawned
   LogSatisfactorySplunkMod: SplunkExporter: loaded config from ../Mods/SatisfactorySplunkMod/Config/SatisfactorySplunkMod.ini
   LogSatisfactorySplunkMod: SplunkExporter: collection started (summary 5s, power 5s, production 30s, inventory 30s, vehicles 10s, players 10s, flush 5s)
   LogSatisfactorySplunkMod: SplunkExporter: HTTP 200 - 1 batches / 12 events sent (0 dropped)
   ```

3. In Splunk, within ~30 seconds:

   ```spl
   index=satisfactory | stats count by sourcetype
   ```

   Expect six sourcetypes: `satisfactory:summary`, `satisfactory:power`,
   `satisfactory:production`, `satisfactory:inventory`,
   `satisfactory:vehicle`, `satisfactory:player`.

## Troubleshooting

| Symptom in log | Cause / fix |
|---|---|
| No `LogSatisfactorySplunkMod` lines at all | Mod not loading — check SML is installed and the folder layout matches Step 1 |
| `config file not found at ...` | Create `Config\SatisfactorySplunkMod.ini` at the exact path shown in the message |
| `SplunkURL / HECToken not configured` | Values still placeholders — edit the ini or set the env vars, then restart |
| `network error - could not reach ...` | Wrong host/port, firewall, or the self-signed HTTPS certificate (use `http://`, see Step 2) |
| `HTTP 401/403` | Token wrong, disabled, or not allowed for the chosen index |
| `HTTP 404` | URL path wrong — it must end in `/services/collector/event` |
| `HTTP 400` | Index named in the ini doesn't exist / isn't allowed for the token |
| Sends OK but nothing in searches | You're searching the wrong index — try `index=* sourcetype=satisfactory:*` |

Still stuck? The mod logs every failure with a specific reason — grab the
`LogSatisfactorySplunkMod` lines and start from there.
