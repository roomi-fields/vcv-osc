# vcv-osc — OSC control for VCV Rack 2

A [VCV Rack 2](https://vcvrack.com) plugin that exposes the **whole patch** to
an external OSC client. Drop the **OSC Controller** module into any patch and
you can, from Python / [osc-bridge](https://github.com/roomi-fields/osc-bridge)
/ Max / a shell script:

- **set / get any parameter** of any module (knobs, buttons, switches), by
  numeric id **or by name** (`"Fundamental/VCO"` / `"Frequency"`);
- **add / remove cables** between any two ports;
- **add / remove modules** and load / save their presets;
- **dump the full patch state** (modules, params, cables, named I/O);
- **subscribe** to parameter *and* structural (module/cable) changes;
- **auto-build a control surface** in TouchOSC / Open Stage Control via a served
  [OSCQuery](https://github.com/Vidvox/OSCQueryProposal) namespace.

It is **wire-compatible with osc-bridge's passthrough surface**: the bridge
strips its `/vcv` prefix and forwards to UDP 7770, replies come back on 7771 —
exactly the ports this module uses by default.

---

## Why a module (and not an "API")

VCV Rack has no official external control API. The only way in is a plugin
loaded *inside Rack's process* that opens an OSC server and translates messages
into calls to Rack's internal APIs. That is what this module is.

Two very different reliability tiers, kept strictly separated in the code:

| Feature  | Rack API used                     | Stability |
|----------|-----------------------------------|-----------|
| Params / state dump / OSCQuery | `engine::Engine` (`setParamValue`/`getParamValue`, `getModule`, `getModuleIds`) | **Public, stable.** |
| Cables   | `app::RackWidget` / `CableWidget` / `PortWidget` | **Unofficial, may break between Rack versions.** |
| Module add/remove, presets | `app::RackWidget` / `ModuleWidget` / `plugin::getModel` | **Unofficial, may break between Rack versions.** |

The fragile app-layer code is quarantined in the `applyCable*` and `applyModule*`
/ `applyPreset*` methods of `src/OscController.cpp`, whose logic follows Rack
2.6.1's own source. If a future Rack breaks it, that is the one place to fix; the
stable engine features (params, dump, OSCQuery, watch) keep working regardless.
See [§ Fragility](#fragility-of-the-cable-api).

---

## Architecture

Four separated layers, as the threading model demands:

```
 network thread                         UI thread (ModuleWidget::step)
┌────────────────┐   thread-safe    ┌──────────────────────────────────┐
│  OscServer     │   CommandQueue   │  OscController::processPending()  │
│  recvfrom loop │ ───────────────► │   ├─ params  → engine API         │
│  parse packet  │   (mutex deque)  │   ├─ cables  → app/UI API  ⚠      │
└────────────────┘                  │   ├─ dump    → engine API         │
        ▲                           │   └─ replies → OscSender ─┐       │
        │ UDP 7770 (listen)         └───────────────────────────┼───────┘
   OSC client / osc-bridge                UDP 7771 (reply) ◄─────┘
```

- **`src/osc/OscServer.{hpp,cpp}`** — owns a UDP socket + a background thread
  that blocks on `recvfrom()`, parses each datagram (message *or* `#bundle`)
  and pushes the result onto the queue. It **never touches Rack**.
- **`src/osc/CommandQueue.hpp`** — a mutex-guarded deque handing messages from
  the network thread to the UI thread.
- **`OscController::processPending()`** — drained every frame from
  `OscControllerWidget::step()` (the UI thread). **Cables are only ever created
  or removed here**, because touching them off the UI thread crashes Rack.
- **`src/osc/OscSender.hpp`** + **`OscMessage.hpp`** — a send-only socket and a
  dependency-free OSC 1.0 codec (int32 / float32 / string, plus bundle
  unpacking). No oscpack / liblo needed.
- **`src/osc/HttpServer.{hpp,cpp}`** — a minimal HTTP server on its own thread,
  serving the OSCQuery namespace. It only reads a cached JSON string that the UI
  thread rebuilds; it **never touches Rack** directly.

### Threading, explicitly

The audio thread (`process()`) does nothing but decay the two activity LEDs.
The network and HTTP threads only read/enqueue. All engine and app reads/writes
happen on the UI thread — the OSCQuery JSON is built there and handed to the HTTP
thread as a string behind a mutex. The receive loop uses a 200 ms socket timeout to poll its stop flag,
because closing a socket does **not** reliably interrupt a blocked `recvfrom()`
on Linux — without that, removing the module would hang Rack. (Found and fixed
via the end-to-end test in `test/`.)

---

## OSC address scheme

Addresses arrive **without** the `/vcv` prefix (osc-bridge strips it; if you
talk to the module directly you may include `/vcv` and it will be tolerated).
Replies are sent to the reply host:port (default `127.0.0.1:7771`).

### Incoming (client → module)

| Address | Args | Effect |
|---|---|---|
| `/param/set`    | `moduleId:int paramId:int value:float` | Set a parameter (clamped to its range). |
| `/param/get`    | `moduleId:int paramId:int`             | → `/param/value` reply. |
| `/param/watch`  | `moduleId:int paramId:int [on:int=1]`  | Subscribe/unsubscribe to changes. |
| `/cable/add`    | `outMod:int outPort:int inMod:int inPort:int` | Add a cable. → `/cable/added` or `/error`. |
| `/cable/remove` | `inMod:int inPort:int`                 | Remove the cable feeding that input. → `/cable/removed`. |
| `/cable/remove_id` | `cableId:int`                       | Remove a cable by id. |
| `/module/add`   | `pluginSlug:str modelSlug:str [x:float y:float]` | Instantiate a module. → `/module/added`. |
| `/module/remove`| `module`                                | Delete a module (and its cables). → `/module/removed`. |
| `/module/preset_save` | `module path:str`                 | Save the module's preset to a file. |
| `/module/preset_load` | `module path:str`                 | Load a preset file into the module (undoable). |
| `/param/<moduleId>/<paramId>` | `[value:float]`           | RESTful per-address set (with arg) / get (OSCQuery form). |
| `/state/dump`   | `[includeParams:int=1] [includePorts:int=1]` | Emit the whole patch (see below). |
| `/state/watch`  | `[on:int=1]`                            | Subscribe to structural changes → `/event/*`. |
| `/registry/dump`| –                                       | List every installed module (module-level, no ports). |
| `/ping`         | –                                       | → `/pong`. |

Every `moduleId` / `paramId` / `portId` above accepts **either** a numeric id
**or** a name string — see [Symbolic addressing](#symbolic-addressing).

### Outgoing (module → client, on the reply port)

| Address | Args |
|---|---|
| `/param/value`  | `moduleId paramId value` |
| `/cable/added`  | `cableId outMod outPort inMod inPort` |
| `/cable/removed`| `cableId [inMod inPort]` |
| `/module/added` | `moduleId pluginSlug modelSlug` |
| `/module/removed`| `moduleId` |
| `/module/preset_saved` | `moduleId path` |
| `/module/preset_loaded`| `moduleId path` |
| `/state/module` | `moduleId pluginSlug modelSlug numParams numInputs numOutputs x y modelName description` |
| `/state/param`  | `moduleId paramId value min max label unit description` |
| `/state/input`  | `moduleId portId name description` |
| `/state/output` | `moduleId portId name description` |
| `/state/cable`  | `cableId outMod outPort inMod inPort` |
| `/state/done`   | `numModules numCables` |
| `/registry/model` | `pluginSlug modelSlug name description` |
| `/registry/done`| `count` |
| `/event/module_add`   | `moduleId pluginSlug modelSlug` |
| `/event/module_remove`| `moduleId` |
| `/event/cable_add`    | `cableId outMod outPort inMod inPort` |
| `/event/cable_remove` | `cableId` |
| `/pong`         | – |
| `/error`        | `message:string` |

Module and port ids come from `/state/dump`. The OSC Controller module itself
appears in the dump like any other module. The `/event/*` messages are only
sent after `/state/watch 1`.

### Symbolic addressing

Numeric ids are canonical but fragile: they only stay valid as long as the
patch is not rebuilt, and they are unreadable in a performance mapping. So
**anywhere an id is expected you may send a name string instead** (the module
branches on the OSC argument *type* — an `int` is an id, a `string` is a name):

| Ref | String form | Example |
|---|---|---|
| Module | `modelSlug` or `pluginSlug/modelSlug`, optional `:N` for the Nth instance | `"VCO"`, `"Fundamental/VCO"`, `"VCO:1"` |
| Param  | the knob **label**, case-insensitive (exact, else first substring) | `"Frequency"` |
| Port   | the **port name**, case-insensitive | `"Sine"`, `"Pitch"` |

```bash
python client/vcv_osc_client.py set "Fundamental/VCO" "Frequency" 0.5
python client/vcv_osc_client.py cable-add "VCO" "Sine" "VCF" "Audio"
```

Names come straight from `/state/dump` (`modelSlug`, param `label`, port
`name`), so a client can discover them once and then address by name forever.
This is the one axis where vcv-osc is strictly more robust than id-only control
surfaces.

### OSCQuery discovery (auto-build a control surface)

The module also serves an [**OSCQuery**](https://github.com/Vidvox/OSCQueryProposal)
namespace over HTTP (default port **7772**). OSCQuery is the standard by which a
controller — **TouchOSC**, **Open Stage Control**, Vezér, Max — discovers an OSC
device and auto-generates a labelled, correctly-ranged surface. Point it at
`http://<rack-host>:7772/` and every module parameter appears as a fader with
its real name and range — no manual mapping.

The namespace exposes each param as a read/write leaf:

```
GET http://127.0.0.1:7772/            → full namespace tree (JSON)
GET http://127.0.0.1:7772/?HOST_INFO  → { NAME, OSC_PORT: 7770, OSC_TRANSPORT: "UDP", … }
GET http://127.0.0.1:7772/param/12    → just module 12's params

/param/<moduleId>/<paramId>  →  { "TYPE":"f", "ACCESS":3,
                                  "RANGE":[{"MIN":…,"MAX":…}], "VALUE":[…],
                                  "DESCRIPTION":"Frequency" }
```

Those leaf addresses are also live **OSC** addresses: send a float to
`/param/12/0` (UDP 7770) to set that param — the RESTful form OSCQuery clients
use, alongside the verb form `/param/set 12 0 <value>`. Inspect the tree from
the shell:

```bash
python client/vcv_osc_client.py oscquery         # print the namespace + HOST_INFO
python client/vcv_osc_client.py addr 12 0 0.8    # set via /param/12/0
```

Enable/disable it and pick the port from the module's right-click menu.

### Naming & typing of I/O

VCV ports carry no signal *type* (gate / CV / audio) — every port is the same
±10 V polyphonic voltage. A port's meaning lives entirely in its **name** and
**description**, which `/state/input` and `/state/output` expose (the
description is the module author's "comment"). Parameters likewise expose
`label`, `unit` and `description`.

There is **no static catalogue** of every module's I/O: a module's ports and
params only exist once it is instantiated. `/registry/dump` lists every
*installed* module at the module level (slug, name, one-line description);
`/state/dump` gives the full per-port / per-param detail for the modules
actually in the open patch.

---

## Build

Requires the [Rack SDK](https://vcvrack.com/downloads) 2.x for your target OS
and a C++ toolchain. Set `RACK_DIR` to the SDK.

```bash
# Linux (native) / macOS (native)
make RACK_DIR=/path/to/Rack-SDK
make RACK_DIR=/path/to/Rack-SDK install     # copies into Rack's user plugin dir

# Windows — build with MSYS2 mingw-w64 and a Windows Rack SDK:
make RACK_DIR=/path/to/Rack-SDK-win

# Cross-compile a Windows .dll from Linux (mingw-w64 + a Windows Rack SDK,
# i.e. one containing libRack.dll.a). This is the verified build path:
make RACK_DIR=/path/to/Rack-SDK CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++
# Package a .vcvplugin (pass the mingw strip so dist can strip the PE binary):
make RACK_DIR=/path/to/Rack-SDK CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ \
     STRIP=x86_64-w64-mingw32-strip dist
```

Output is `plugin.{so,dylib,dll}`. `make dist` produces a `.vcvplugin` package.

The target platform (lin/mac/win) is auto-detected from the compiler; the only
platform-specific code is the UDP socket wrapper (`src/osc/UdpSocket.cpp`),
which uses Winsock on Windows and BSD sockets elsewhere.

### Install manually

Copy the built plugin folder into your Rack user directory:

- Linux: `~/.local/share/Rack2/plugins-lin-x64/vcv-osc/`
- macOS: `~/Library/Application Support/Rack2/plugins-mac-<arch>/vcv-osc/`
- Windows: `%LOCALAPPDATA%\Rack2\plugins-win-x64\vcv-osc\`

`make install` does this for you.

---

## Use

1. Add **OSC Controller** to your patch (search "OSC" in the module browser).
   Its display shows the listen status, reply target and a message counter.
   Right-click to change the listen/reply ports.
2. From a client, dump the patch to discover module and port ids:

```bash
pip install python-osc
python client/vcv_osc_client.py dump
```

3. Then drive it:

```bash
python client/vcv_osc_client.py set 12 0 0.8           # knob param 0 of module 12
python client/vcv_osc_client.py press 12 3             # momentary button
python client/vcv_osc_client.py cable-add 12 0 15 1    # out(mod12,port0) → in(mod15,port1)
python client/vcv_osc_client.py cable-remove 15 1
python client/vcv_osc_client.py watch 12 0             # stream knob changes
python client/vcv_osc_client.py demo 12 0              # scripted showcase
```

See `client/vcv_osc_client.py --help` for all commands and the `--host/--port/
--reply` options.

### Driving it through osc-bridge

The module is registered as osc-bridge's VCV Rack passthrough target
(`devices/vcv-rack/…`). Point osc-bridge at it and prefix addresses with `/vcv`:

```bash
osc-bridge run --device devices/vcv-rack/vcv-rack.third-party-osc.fw-2.5.json
osc-bridge osc-send /vcv/param/set 12 0 0.8
osc-bridge osc-send /vcv/state/dump 1
```

osc-bridge strips `/vcv`, forwards to UDP 7770, and re-prefixes replies coming
back on 7771.

---

## Test

An end-to-end test of the OSC layer (no Rack required) compiles the networking
code into a standalone server and drives it from python-osc, proving the wire
format matches:

```bash
pip install python-osc
bash test/run.sh          # prints RESULT: PASS
```

---

## Security

This module is an **unauthenticated remote-control surface**. Anyone who can
reach its ports can drive the patch. Specifically:

- The OSC server (UDP 7770) and the OSCQuery HTTP server (TCP 7772) bind on
  **all interfaces** — deliberately, so a phone/tablet running TouchOSC can
  reach Rack over the LAN. There is no password.
- `/module/preset_load` and `/module/preset_save` **read and write files by
  path** on the machine running Rack.

This is fine on `localhost` or a trusted studio LAN (the intended use). On an
untrusted network, firewall ports 7770/7772 or simply don't add the module.
Everything is off when no OSC Controller module is in the patch.

## Fragility of the cable API

**The cable feature relies on Rack's app/UI layer, which is explicitly not part
of the stable plugin ABI.** VCV documents that filenames and symbol locations
can change in any Rack version. Concretely, this plugin depends on:

- `APP->scene->rack` being an `app::RackWidget` with `getModule`, `addCable`,
  `removeCable`, `getTopCable`, `getCompleteCablesOnPort`, `getNextCableColor`;
- `app::CableWidget` exposing `inputPort` / `outputPort` and `updateCable()`;
- `app::ModuleWidget::getInput/getOutput(portId)`;
- `history::CableAdd` / `history::CableRemove` for undo integration.

All of this was verified against **Rack SDK 2.6.1** and mirrors Rack's own
source. Between the mission's original snippet (`cw->setInput(...)`) and 2.6.1
the API already changed to `updateCable()`/`setCable()` — a reminder that this
*will* drift. Parameters, by contrast, use the stable engine API and are safe.

If Rack breaks the cable API, params, get/set, dump and watch keep working; only
`applyCableAdd/Remove/RemoveId` need updating.

## License

GPL-3.0-or-later (matches VCV Rack and osc-bridge).
