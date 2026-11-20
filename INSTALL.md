# Installing the Satisfactory Splunk Exporter

This guide covers getting the **built** mod into the game and talking to
Splunk. To compile it, see "Building the mod" in [README.md](README.md).

> **Rebuild after game patches.** A C++ mod only loads in the game build it
> was compiled against. If your last build is from before the current patch,
> `git pull` the SatisfactoryModLoader project, install the engine version
> named in its `FactoryGame.uproject` (currently `5.6.1-CSS`), and rebuild
> before following this guide.

## Requirements

- Satisfactory, current patch (mod verified against game build CL517554)
- [Satisfactory Mod Manager (SMM)](https://smm.ficsit.app/) or a manual SML
  install, with SML **3.12.0 or newer**
- A Splunk instance with an HTTP Event Collector (HEC) token
- Multiplayer clients do **not** need the mod. It only exports on the
  host/dedicated server.

## Step 1: Prove Splunk works (no game needed)

1. In Splunk: **Settings → Data Inputs → HTTP Event Collector → New Token**.
   Pick or create an events index (e.g. `satisfactory`) and copy the token.
2. Check **HTTP Event Collector → Global Settings**: *All Tokens* must be
   **Enabled**.
3. **Certificate warning (the #1 failure cause):** Splunk's default HEC
   certificate is self-signed and the game rejects it. On a home/LAN setup,
   uncheck **Enable SSL** in the same Global Settings dialog and use an
   `http://` URL. (Alternative: a certificate your OS trusts.)
4. From the PC that runs the game, open PowerShell in this repository's
   folder and run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\Test-SplunkHec.ps1 -Url http://YOUR-SPLUNK:8088/services/collector/event -Token YOUR-TOKEN
   ```

   This sends events exactly the way the mod does. You must see
   `HTTP 200`. If not, the script tells you what's wrong; fix that first,
   because the mod will hit the same error.

## Step 2: Install the mod files

### Option A: Alpakit "Copy to game"

If you set your game path in Alpakit and packaged with *Copy to Game*
enabled, the mod is already at `<game>\FactoryGame\Mods\SatisfactorySplunkMod\`.

### Option B: Manual copy of the packaged zip

Alpakit writes a zip per target (e.g. `Windows`) into the modding project's
`Saved\ArchivedPlugins\` folder. Extract it so you end up with:

```
<game>\FactoryGame\Mods\SatisfactorySplunkMod\
├── SatisfactorySplunkMod.uplugin
├── Binaries\
├── Content\
└── Config\
    ├── SatisfactorySplunkMod.ini
    └── PluginSettings.ini
```

Typical game locations:

- Steam: `C:\Program Files (x86)\Steam\steamapps\common\Satisfactory`
- Epic: `C:\Program Files\Epic Games\Satisfactory`

### Option C: Dedicated server

Copy the **server** package (`WindowsServer` or `LinuxServer`) to the
server's `FactoryGame/Mods/` folder. SML must be installed on the server too
(SMM can manage servers, or use
[ficsit-cli](https://github.com/satisfactorymodding/ficsit-cli)).

## Step 3: First launch creates the config file

1. Launch the game and **load a save**.
2. The mod creates its config file at:

   ```
   <game>\FactoryGame\Configs\SatisfactorySplunkMod.ini
   ```

   and logs that it isn't configured yet. That's expected.
3. Quit the game, open that file, set:

   ```ini
   SplunkURL=http://YOUR-SPLUNK:8088/services/collector/event
   HECToken=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
   ```

   (Optionally `Index=satisfactory`, if the token's default index isn't the one
   you want.)
4. You can re-run the Step 1 test straight from this file:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\Test-SplunkHec.ps1 -ConfigPath "<game>\FactoryGame\Configs\SatisfactorySplunkMod.ini"
   ```

This file lives outside the mod folder on purpose: SMM replaces the mod
folder on every update, which would wipe your token.

**Dedicated servers:** you can skip the file and set environment variables
instead (they override it): `SPLUNK_HEC_URL`, `SPLUNK_HEC_TOKEN`,
`SPLUNK_HEC_INDEX`.

## Step 4: Verify

1. Launch the game and load a save.
2. Open `<game>\FactoryGame\Saved\Logs\FactoryGame.log` and search for
   `LogSatisfactorySplunkMod`. A healthy start looks like:

   ```
   LogSatisfactorySplunkMod: SplunkGameWorldModule: POST_INITIALIZATION received
   LogSatisfactorySplunkMod: SplunkGameWorldModule: SplunkExporter spawned
   LogSatisfactorySplunkMod: SplunkExporter 2.1.0: starting up on 'YOUR-PC'
   LogSatisfactorySplunkMod: SplunkExporter: config 'C:/.../FactoryGame/Configs/SatisfactorySplunkMod.ini' -> URL 'http://...', token 'abcd...', index '(token default)'
   LogSatisfactorySplunkMod: SplunkExporter: collection started (summary 5s, power 5s, ...)
   LogSatisfactorySplunkMod: SplunkExporter: HTTP 200 OK - 1 batches / 1 events sent (0 dropped)
   ```

   The `HTTP 200` line should appear within a second or two of loading. It's
   the startup event.
3. In Splunk:

   ```spl
   index=* sourcetype=satisfactory:* | stats count by sourcetype
   ```

   `satisfactory:status` appears immediately, `summary`/`power`/`player`/`vehicle`
   after ~3 seconds, and `production`/`inventory` on their first run as well.

## Troubleshooting

Find the **first** `LogSatisfactorySplunkMod` line that isn't in the healthy
list above:

| Log shows | Meaning / fix |
|---|---|
| **No `LogSatisfactorySplunkMod` lines at all** | The mod isn't loading. Check SML is installed; search the log for `SatisfactorySplunkMod` to see if SML rejected it (wrong game version means you need to rebuild against the current patch); check the folder layout in Step 2 |
| Mod loads but no `POST_INITIALIZATION received` | Search the log for `Discovered` and `world modules`. Send me those lines |
| `created config file ...` then `NOT STARTED - ... placeholders` | Normal on first launch. Do Step 3 |
| `NOT STARTED - SplunkURL must start with http://` | Fix the URL in the config |
| `network error - could not reach ...` | Wrong host/port, firewall, or HTTPS with a self-signed cert. The Step 1 script reproduces this outside the game |
| `HTTP 401` / `403` | Token wrong or disabled |
| `HTTP 404` | URL must end in `/services/collector/event` |
| `HTTP 400` | Usually the `Index` isn't allowed for this token. Clear `Index=` or allow it on the token |
| `HTTP 503` | HEC disabled in Global Settings, or Splunk overloaded |
| `HTTP 200 OK` but searches find nothing | Search `index=*`. The data went to the token's default index |
| `running as a multiplayer client` | You joined someone else's game. Only the host/server exports |

If you're stuck, send the `LogSatisfactorySplunkMod` lines (plus any lines
mentioning `SatisfactorySplunkMod`). Every failure path logs a specific
reason.
