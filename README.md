# vcv-osc — OSC control for VCV Rack 2

A [VCV Rack 2](https://vcvrack.com) plugin that exposes the **whole patch** to
an external OSC client. Drop the **OSC Controller** module into any patch and
you can, from Python / [osc-bridge](https://github.com/roomi-fields/osc-bridge)
/ Max / a shell script:

- **set / get any parameter** of any module (knobs, buttons, switches);
- **add / remove cables** between any two ports;
- **dump the full patch state** (modules, params, cables);
- **subscribe** to parameter changes for bidirectional control (bonus).

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
| Params   | `engine::Engine` (`setParamValue`/`getParamValue`, `getModule`) | **Public, stable.** |
| Cables   | `app::RackWidget` / `CableWidget` / `PortWidget` | **Unofficial, may break between Rack versions.** |

The cable code is quarantined in three methods (`applyCableAdd/Remove/RemoveId`
in `src/OscController.cpp`) whose logic is copied verbatim from Rack 2.6.1's own
source (`RackWidget.cpp`, `PortWidget.cpp`). If a future Rack breaks it, that is
the one place to fix. See [§ Fragility](#fragility-of-the-cable-api).

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

### Threading, explicitly

The audio thread (`process()`) does nothing but decay the two activity LEDs.
The network thread only enqueues. All engine and app reads/writes happen on the
UI thread. The receive loop uses a 200 ms socket timeout to poll its stop flag,
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
| `/state/dump`   | `[includeParams:int=1] [includePorts:int=1]` | Emit the whole patch (see below). |
| `/registry/dump`| –                                       | List every installed module (module-level, no ports). |
| `/ping`         | –                                       | → `/pong`. |

### Outgoing (module → client, on the reply port)

| Address | Args |
|---|---|
| `/param/value`  | `moduleId paramId value` |
| `/cable/added`  | `cableId outMod outPort inMod inPort` |
| `/cable/removed`| `cableId [inMod inPort]` |
| `/state/module` | `moduleId pluginSlug modelSlug numParams numInputs numOutputs x y modelName description` |
| `/state/param`  | `moduleId paramId value min max label unit description` |
| `/state/input`  | `moduleId portId name description` |
| `/state/output` | `moduleId portId name description` |
| `/state/cable`  | `cableId outMod outPort inMod inPort` |
| `/state/done`   | `numModules numCables` |
| `/registry/model` | `pluginSlug modelSlug name description` |
| `/registry/done`| `count` |
| `/pong`         | – |
| `/error`        | `message:string` |

Module and port ids come from `/state/dump`. The OSC Controller module itself
appears in the dump like any other module.

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
