#include "plugin.hpp"
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>

#include "osc/OscMessage.hpp"
#include "osc/CommandQueue.hpp"
#include "osc/OscServer.hpp"
#include "osc/OscSender.hpp"
#include "osc/HttpServer.hpp"

using namespace vcvosc;

/* ============================================================================
 * vcv-osc — OSC Controller module
 *
 * Remote-controls the entire Rack patch from an external OSC client (Python,
 * osc-bridge, Max, …). One module instance runs an OSC server; the addressing
 * scheme is documented in README.md and mirrors the osc-bridge passthrough
 * surface (osc-bridge strips its own `/vcv` prefix, so messages arrive here as
 * `/param/set`, `/cable/add`, `/state/dump`, …).
 *
 * ARCHITECTURE — four clearly separated layers, as required:
 *   1. OSC server (network thread)  → osc/OscServer.{hpp,cpp}
 *   2. Thread-safe command queue    → osc/CommandQueue.hpp
 *   3. UI-thread applicator         → OscController::processPending() below,
 *                                      driven from OscControllerWidget::step()
 *   4a. Parameter access            → applyParamSet/Get (engine API, stable)
 *   4b. Cable access                → applyCableAdd/Remove (app/UI API, FRAGILE
 *                                      — isolated in this one place so a future
 *                                      Rack API break is a localized fix)
 *
 * THREADING — the network thread NEVER touches Rack. It only enqueues parsed
 * messages. Everything that reads/writes engine or app state runs on the UI
 * thread inside processPending(), because cables must be manipulated there.
 * ==========================================================================*/

struct OscController : Module {
	enum ParamId { PARAMS_LEN };
	enum InputId { INPUTS_LEN };
	enum OutputId { OUTPUTS_LEN };
	enum LightId { RX_LIGHT, TX_LIGHT, LIGHTS_LEN };

	// --- Configuration (persisted) ---
	uint16_t listenPort = 7770;         // osc-bridge forwards here (prefix stripped)
	std::string replyHost = "127.0.0.1";
	uint16_t replyPort = 7771;          // osc-bridge reply_port
	bool notifyOnSet = true;            // echo /param/value after a /param/set
	bool oscQueryEnabled = true;        // serve the OSCQuery namespace over HTTP
	uint16_t oscQueryPort = 7772;       // HTTP port for OSCQuery discovery

	// --- Runtime ---
	CommandQueue queue;
	std::unique_ptr<OscServer> server;
	OscSender sender;

	// OSCQuery: a small HTTP server serves a JSON description of the live patch's
	// OSC namespace (Phase 2). The tree is (re)built on the UI thread and cached
	// as a string behind a mutex; the HTTP thread only reads that string.
	std::unique_ptr<HttpServer> httpServer;
	std::mutex oscQueryMutex;
	std::string oscQueryDoc;            // cached namespace JSON
	int oscQueryTick = 0;               // frame counter to throttle rebuilds
	bool httpOk = false;
	std::string httpStatus = "off";

	std::atomic<bool> configDirty{true};   // (re)start server on next UI step
	std::atomic<bool> rxActivity{false};
	std::atomic<bool> txActivity{false};
	std::atomic<int> messageCount{0};
	bool serverOk = false;
	std::string serverStatus = "starting…";

	// Bonus: parameter change watches for bidirectional control.
	struct Watch { int64_t moduleId; int paramId; float last; };
	std::vector<Watch> watches;

	// Structural change watch: stream module/cable add/remove events. We detect
	// changes by diffing a snapshot of module & cable ids each UI frame — no
	// hook into Rack internals, so nothing to break when Rack changes.
	bool structWatch = false;
	std::vector<int64_t> lastModuleIds;   // kept sorted
	std::vector<int64_t> lastCableIds;    // kept sorted

	OscController() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		server = std::unique_ptr<OscServer>(new OscServer(queue));
		httpServer = std::unique_ptr<HttpServer>(new HttpServer);
	}

	~OscController() override {
		if (server)
			server->stop();
		if (httpServer)
			httpServer->stop();
	}

	// ------------------------------------------------------------------ audio
	// The audio thread does nothing but decay the activity LEDs. It must never
	// touch cables or the network.
	void process(const ProcessArgs& args) override {
		const float decay = args.sampleTime * 4.f;
		lights[RX_LIGHT].setBrightness(rxActivity.exchange(false) ? 1.f
			: std::max(0.f, lights[RX_LIGHT].getBrightness() - decay));
		lights[TX_LIGHT].setBrightness(txActivity.exchange(false) ? 1.f
			: std::max(0.f, lights[TX_LIGHT].getBrightness() - decay));
	}

	// --------------------------------------------------------------- UI thread
	// Called from OscControllerWidget::step(); safe context for engine + app API.
	void processPending() {
		if (configDirty.exchange(false))
			restartServer();

		for (OscMessage& m : queue.drain()) {
			messageCount.fetch_add(1);
			rxActivity.store(true);
			dispatch(m);
		}

		pollWatches();
		pollStructure();

		// Refresh the cached OSCQuery namespace periodically so VALUE fields track
		// the patch (every ~30 frames ≈ 0.5 s; cheap for typical patches).
		if (httpOk && ++oscQueryTick >= 30) {
			oscQueryTick = 0;
			rebuildOscQuery();
		}
	}

	void requestRestart() { configDirty.store(true); }

	void restartServer() {
		serverOk = server->start(listenPort);
		serverStatus = serverOk ? string::f("listening :%d", (int) listenPort)
		                        : ("error: " + server->lastError());
		sender.setTarget(replyHost, replyPort);

		// (Re)start the OSCQuery HTTP server.
		httpServer->stop();
		httpOk = false;
		httpStatus = "off";
		if (oscQueryEnabled) {
			rebuildOscQuery();  // have a doc ready before the first HTTP request
			httpOk = httpServer->start(oscQueryPort,
				[this](const std::string& path, const std::string& query) {
					return handleOscQuery(path, query);
				});
			httpStatus = httpOk ? string::f("OSCQuery :%d", (int) oscQueryPort)
			                    : ("http error: " + httpServer->lastError());
		}
	}

	// ---------------------------------------------------------------- dispatch
	void dispatch(const OscMessage& msg) {
		// Tolerate a leading "/vcv" so the module also works when a client talks
		// to it directly (not through osc-bridge, which already strips it).
		std::string addr = msg.address;
		if (addr.rfind("/vcv/", 0) == 0)
			addr = addr.substr(4);

		if (addr == "/param/set")        applyParamSet(msg);
		else if (addr == "/param/get")   applyParamGet(msg);
		else if (addr == "/param/watch") applyParamWatch(msg);
		else if (addr == "/cable/add")   applyCableAdd(msg);
		else if (addr == "/cable/remove") applyCableRemove(msg);
		else if (addr == "/cable/remove_id") applyCableRemoveId(msg);
		else if (addr == "/module/add")    applyModuleAdd(msg);
		else if (addr == "/module/remove") applyModuleRemove(msg);
		else if (addr == "/module/preset_save") applyPresetSave(msg);
		else if (addr == "/module/preset_load") applyPresetLoad(msg);
		else if (addr == "/state/dump")  applyStateDump(msg);
		else if (addr == "/state/watch") applyStructWatch(msg);
		else if (addr == "/registry/dump") applyRegistryDump(msg);
		else if (addr == "/ping")        reply(OscMessage("/pong"));
		// RESTful per-address param access — the shape OSCQuery leaves use:
		//   /param/<moduleId>/<paramId>  [value:float]   set (with arg) / get (none)
		else if (tryParamAddress(addr, msg)) { /* handled */ }
		else sendError("unknown address: " + addr);
	}

	// Handle "/param/<moduleId>/<paramId>" leaf addresses (numeric ids, as emitted
	// in the OSCQuery namespace). Returns false if `addr` isn't of that shape so
	// the caller can fall through to the error path.
	bool tryParamAddress(const std::string& addr, const OscMessage& m) {
		if (addr.rfind("/param/", 0) != 0) return false;
		std::string rest = addr.substr(7); // after "/param/"
		std::string::size_type slash = rest.find('/');
		if (slash == std::string::npos) return false;
		std::string aMod = rest.substr(0, slash);
		std::string aParam = rest.substr(slash + 1);
		if (!isInteger(aMod) || !isInteger(aParam)) return false;

		int64_t moduleId = (int64_t) atoll(aMod.c_str());
		int paramId = atoi(aParam.c_str());
		engine::Module* mod = APP->engine->getModule(moduleId);
		if (!mod) { sendError(string::f("no module %lld", (long long) moduleId)); return true; }
		if (paramId < 0 || paramId >= mod->getNumParams()) {
			sendError(string::f("module %lld has no param %d", (long long) moduleId, paramId));
			return true;
		}
		if (!m.args.empty()) {
			float value = m.args[0].asFloat();
			if (engine::ParamQuantity* pq = mod->getParamQuantity(paramId))
				value = clamp(value, pq->getMinValue(), pq->getMaxValue());
			APP->engine->setParamValue(mod, paramId, value);
			if (notifyOnSet) emitParamValue(moduleId, paramId);
		} else {
			emitParamValue(moduleId, paramId);
		}
		return true;
	}

	static bool isInteger(const std::string& s) {
		if (s.empty()) return false;
		size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
		if (i >= s.size()) return false;
		for (; i < s.size(); i++)
			if (!std::isdigit((unsigned char) s[i])) return false;
		return true;
	}

	// ------------------------------------------------------------ send helpers
	void reply(const OscMessage& m) {
		txActivity.store(true);
		sender.send(m);
	}
	void sendError(const std::string& text) {
		OscMessage m("/error");
		m.pushString(text);
		reply(m);
	}

	// ---------------------------------------------- SYMBOLIC ADDRESSING (Phase 1)
	// Every id argument may arrive as an int (the canonical engine/index id, as
	// before) OR as a string (a stable, human-memorable symbolic reference). We
	// branch on the OSC arg *type*, so the wire stays fully backward-compatible:
	// old numeric clients are untouched; new clients can address by name.
	//
	//   module : "modelSlug" | "pluginSlug/modelSlug", optionally ":index" to pick
	//            the Nth instance (0-based, ordered by id) when several match.
	//   param  : the ParamQuantity label (e.g. "Frequency"), case-insensitive,
	//            exact match preferred, else first substring match.
	//   port   : the PortInfo name (e.g. "Pitch"), same matching as params.
	//
	// Symbolic refs are robust to the thing numeric ids are not: rebuilding the
	// patch from scratch, and being readable in a performance mapping.
	static std::string toLower(std::string s) {
		for (char& c : s) c = (char) std::tolower((unsigned char) c);
		return s;
	}
	static bool ciEq(const std::string& a, const std::string& b) {
		return toLower(a) == toLower(b);
	}

	// Resolve a module argument (int id or symbolic string) → engine module id,
	// or -1 with `err` set.
	int64_t resolveModule(const OscArg& a, std::string& err) {
		if (a.type != OscArg::STRING) {
			int64_t id = a.asInt();
			if (!APP->engine->getModule(id)) {
				err = string::f("no module %lld", (long long) id);
				return -1;
			}
			return id;
		}

		std::string ref = a.s;
		int index = 0;
		std::string::size_type colon = ref.rfind(':');
		if (colon != std::string::npos) {
			index = atoi(ref.c_str() + colon + 1);
			ref = ref.substr(0, colon);
		}
		std::string wantPlugin, wantModel;
		std::string::size_type slash = ref.find('/');
		if (slash != std::string::npos) {
			wantPlugin = ref.substr(0, slash);
			wantModel = ref.substr(slash + 1);
		} else {
			wantModel = ref;
		}

		std::vector<int64_t> matches = APP->engine->getModuleIds();
		std::sort(matches.begin(), matches.end()); // deterministic instance order
		std::vector<int64_t> hits;
		for (int64_t id : matches) {
			engine::Module* mod = APP->engine->getModule(id);
			if (!mod) continue;
			plugin::Model* model = mod->getModel();
			if (!model) continue;
			// Match the model by slug (exact) OR human name (case-insensitive),
			// symmetric with how params/ports match their label.
			if (model->slug != wantModel && !ciEq(model->name, wantModel)) continue;
			if (!wantPlugin.empty()) {
				plugin::Plugin* pl = model->plugin;
				if (!pl || (pl->slug != wantPlugin && !ciEq(pl->name, wantPlugin)))
					continue;
			}
			hits.push_back(id);
		}
		if (hits.empty()) {
			err = "no module matching '" + a.s + "'";
			return -1;
		}
		if (index < 0 || index >= (int) hits.size()) {
			err = string::f("module '%s': index %d out of %d match(es)",
			                a.s.c_str(), index, (int) hits.size());
			return -1;
		}
		return hits[index];
	}

	// Resolve a param argument (int index or label string) → param index, or -1.
	int resolveParam(engine::Module* mod, const OscArg& a, std::string& err) {
		if (a.type != OscArg::STRING) {
			int p = a.asInt();
			if (p < 0 || p >= mod->getNumParams()) {
				err = string::f("module %lld has no param %d", (long long) mod->id, p);
				return -1;
			}
			return p;
		}
		std::string want = toLower(a.s);
		int substr = -1;
		for (int p = 0; p < mod->getNumParams(); p++) {
			engine::ParamQuantity* pq = mod->getParamQuantity(p);
			std::string label = toLower(pq ? pq->getLabel() : "");
			if (label == want) return p;                       // exact wins
			if (substr < 0 && !want.empty() && label.find(want) != std::string::npos)
				substr = p;
		}
		if (substr >= 0) return substr;
		err = string::f("module %lld has no param named '%s'", (long long) mod->id, a.s.c_str());
		return -1;
	}

	// Resolve a port argument (int index or PortInfo-name string) → port index.
	int resolvePort(engine::Module* mod, const OscArg& a, bool output, std::string& err) {
		int count = output ? mod->getNumOutputs() : mod->getNumInputs();
		const char* kind = output ? "output" : "input";
		if (a.type != OscArg::STRING) {
			int p = a.asInt();
			if (p < 0 || p >= count) {
				err = string::f("module %lld has no %s %d", (long long) mod->id, kind, p);
				return -1;
			}
			return p;
		}
		std::string want = toLower(a.s);
		int substr = -1;
		for (int p = 0; p < count; p++) {
			engine::PortInfo* pi = output ? mod->getOutputInfo(p) : mod->getInputInfo(p);
			std::string name = toLower(pi ? pi->getName() : "");
			if (name == want) return p;
			if (substr < 0 && !want.empty() && name.find(want) != std::string::npos)
				substr = p;
		}
		if (substr >= 0) return substr;
		err = string::f("module %lld has no %s named '%s'", (long long) mod->id, kind, a.s.c_str());
		return -1;
	}

	// -------------------------------------------------------- 4a. PARAM access
	// Engine API is public + stable. Safe from any thread, but we stay on the UI
	// thread for consistent ordering with cable ops.
	void applyParamSet(const OscMessage& m) {
		if (m.args.size() < 3) return sendError("/param/set needs <module> <param> <value>");
		std::string err;
		int64_t moduleId = resolveModule(m.args[0], err);
		if (moduleId < 0) return sendError(err);
		engine::Module* mod = APP->engine->getModule(moduleId);
		int paramId = resolveParam(mod, m.args[1], err);
		if (paramId < 0) return sendError(err);
		float value = m.args[2].asFloat();

		// Clamp to the param's declared range when it has a ParamQuantity.
		if (engine::ParamQuantity* pq = mod->getParamQuantity(paramId))
			value = clamp(value, pq->getMinValue(), pq->getMaxValue());

		APP->engine->setParamValue(mod, paramId, value);

		if (notifyOnSet)
			emitParamValue(moduleId, paramId);
	}

	void applyParamGet(const OscMessage& m) {
		if (m.args.size() < 2) return sendError("/param/get needs <module> <param>");
		std::string err;
		int64_t moduleId = resolveModule(m.args[0], err);
		if (moduleId < 0) return sendError(err);
		int paramId = resolveParam(APP->engine->getModule(moduleId), m.args[1], err);
		if (paramId < 0) return sendError(err);
		emitParamValue(moduleId, paramId);
	}

	void emitParamValue(int64_t moduleId, int paramId) {
		engine::Module* mod = APP->engine->getModule(moduleId);
		if (!mod) return;
		OscMessage out("/param/value");
		out.pushInt((int32_t) moduleId);
		out.pushInt(paramId);
		out.pushFloat(APP->engine->getParamValue(mod, paramId));
		reply(out);
	}

	// Bonus bidirectional control: subscribe/unsubscribe to a param's changes.
	void applyParamWatch(const OscMessage& m) {
		if (m.args.size() < 2) return sendError("/param/watch needs <module> <param> [on=1]");
		std::string err;
		int64_t moduleId = resolveModule(m.args[0], err);
		if (moduleId < 0) return sendError(err);
		int paramId = resolveParam(APP->engine->getModule(moduleId), m.args[1], err);
		if (paramId < 0) return sendError(err);
		bool on = m.args.size() >= 3 ? (m.args[2].asInt() != 0) : true;

		auto it = std::find_if(watches.begin(), watches.end(), [&](const Watch& w) {
			return w.moduleId == moduleId && w.paramId == paramId;
		});
		if (on) {
			if (it == watches.end()) {
				float cur = 0.f;
				if (engine::Module* mod = APP->engine->getModule(moduleId))
					cur = APP->engine->getParamValue(mod, paramId);
				watches.push_back({moduleId, paramId, cur});
				emitParamValue(moduleId, paramId); // send initial value
			}
		}
		else if (it != watches.end()) {
			watches.erase(it);
		}
	}

	void pollWatches() {
		for (Watch& w : watches) {
			engine::Module* mod = APP->engine->getModule(w.moduleId);
			if (!mod || w.paramId < 0 || w.paramId >= mod->getNumParams())
				continue;
			float v = APP->engine->getParamValue(mod, w.paramId);
			if (v != w.last) {
				w.last = v;
				emitParamValue(w.moduleId, w.paramId);
			}
		}
	}

	// Structural watch: /state/watch [on=1]. When on, subsequent topology changes
	// are streamed as /event/{module_add,module_remove,cable_add,cable_remove}.
	// Subscribing just snapshots silently (the client already has /state/dump); we
	// only report deltas from there on.
	void applyStructWatch(const OscMessage& m) {
		bool on = m.args.size() >= 1 ? (m.args[0].asInt() != 0) : true;
		structWatch = on;
		if (on) {
			lastModuleIds = APP->engine->getModuleIds();
			lastCableIds = APP->engine->getCableIds();
			std::sort(lastModuleIds.begin(), lastModuleIds.end());
			std::sort(lastCableIds.begin(), lastCableIds.end());
		}
	}

	void pollStructure() {
		if (!structWatch) return;

		std::vector<int64_t> mods = APP->engine->getModuleIds();
		std::vector<int64_t> cables = APP->engine->getCableIds();
		std::sort(mods.begin(), mods.end());
		std::sort(cables.begin(), cables.end());

		// Modules removed / added (set difference on sorted id lists).
		diffIds(lastModuleIds, mods,
			[&](int64_t id) {                       // removed
				OscMessage e("/event/module_remove");
				e.pushInt((int32_t) id);
				reply(e);
			},
			[&](int64_t id) {                       // added
				OscMessage e("/event/module_add");
				e.pushInt((int32_t) id);
				if (engine::Module* mod = APP->engine->getModule(id)) {
					plugin::Model* model = mod->getModel();
					e.pushString((model && model->plugin) ? model->plugin->slug : "?");
					e.pushString(model ? model->slug : "?");
				} else {
					e.pushString("?"); e.pushString("?");
				}
				reply(e);
			});

		// Cables removed / added.
		diffIds(lastCableIds, cables,
			[&](int64_t id) {                       // removed
				OscMessage e("/event/cable_remove");
				e.pushInt((int32_t) id);
				reply(e);
			},
			[&](int64_t id) {                       // added
				OscMessage e("/event/cable_add");
				e.pushInt((int32_t) id);
				if (engine::Cable* c = APP->engine->getCable(id)) {
					e.pushInt(c->outputModule ? (int32_t) c->outputModule->id : -1);
					e.pushInt(c->outputId);
					e.pushInt(c->inputModule ? (int32_t) c->inputModule->id : -1);
					e.pushInt(c->inputId);
				}
				reply(e);
			});

		lastModuleIds.swap(mods);
		lastCableIds.swap(cables);
	}

	// Walk two sorted id lists, calling onRemoved(id) for ids only in `before`
	// and onAdded(id) for ids only in `after`.
	template <typename FRem, typename FAdd>
	static void diffIds(const std::vector<int64_t>& before,
	                    const std::vector<int64_t>& after,
	                    FRem onRemoved, FAdd onAdded) {
		size_t i = 0, j = 0;
		while (i < before.size() && j < after.size()) {
			if (before[i] < after[j]) { onRemoved(before[i]); i++; }
			else if (before[i] > after[j]) { onAdded(after[j]); j++; }
			else { i++; j++; }
		}
		for (; i < before.size(); i++) onRemoved(before[i]);
		for (; j < after.size(); j++) onAdded(after[j]);
	}

	// --------------------------------------------------- 4b. CABLE access (UI!)
	// FRAGILE LAYER — the app/UI cable API is not part of the stable plugin ABI
	// and may change between Rack versions. All cable manipulation is confined
	// to these three methods so a future break is a one-place fix. Patterns are
	// copied from Rack's own RackWidget/PortWidget source (v2.6.1):
	//  - add:    new CableWidget → set ports → updateCable() (engine) → addCable
	//            (widget) → history::CableAdd
	//  - remove: getTopCable → history::CableRemove → removeCable → delete
	void applyCableAdd(const OscMessage& m) {
		if (m.args.size() < 4)
			return sendError("/cable/add needs <outModule> <outPort> <inModule> <inPort>");
		std::string err;
		int64_t outMod = resolveModule(m.args[0], err);
		if (outMod < 0) return sendError(err);
		int outPort = resolvePort(APP->engine->getModule(outMod), m.args[1], /*output=*/true, err);
		if (outPort < 0) return sendError(err);
		int64_t inMod = resolveModule(m.args[2], err);
		if (inMod < 0) return sendError(err);
		int inPort = resolvePort(APP->engine->getModule(inMod), m.args[3], /*output=*/false, err);
		if (inPort < 0) return sendError(err);

		app::RackWidget* rack = APP->scene->rack;
		app::ModuleWidget* omw = rack->getModule(outMod);
		app::ModuleWidget* imw = rack->getModule(inMod);
		if (!omw) return sendError(string::f("no module widget %lld", (long long) outMod));
		if (!imw) return sendError(string::f("no module widget %lld", (long long) inMod));

		app::PortWidget* opw = omw->getOutput(outPort);
		app::PortWidget* ipw = imw->getInput(inPort);
		if (!opw) return sendError(string::f("module %lld has no output %d", (long long) outMod, outPort));
		if (!ipw) return sendError(string::f("module %lld has no input %d", (long long) inMod, inPort));

		// A Rack input holds a single cable. Refuse rather than silently steal it.
		if (!rack->getCompleteCablesOnPort(ipw).empty())
			return sendError(string::f("input %lld:%d already connected", (long long) inMod, inPort));

		app::CableWidget* cw = new app::CableWidget;
		cw->color = rack->getNextCableColor();
		cw->inputPort = ipw;
		cw->outputPort = opw;
		cw->updateCable();          // creates engine::Cable and adds it to the engine
		if (!cw->isComplete()) {    // defensive: ports vanished mid-op
			delete cw;
			return sendError("failed to create cable");
		}
		rack->addCable(cw);         // adds the widget

		history::CableAdd* h = new history::CableAdd;
		h->setCable(cw);
		APP->history->push(h);

		OscMessage out("/cable/added");
		out.pushInt((int32_t) cw->getCable()->id);
		out.pushInt((int32_t) outMod);
		out.pushInt(outPort);
		out.pushInt((int32_t) inMod);
		out.pushInt(inPort);
		reply(out);
	}

	void applyCableRemove(const OscMessage& m) {
		if (m.args.size() < 2)
			return sendError("/cable/remove needs <inModule> <inPort>");
		std::string err;
		int64_t inMod = resolveModule(m.args[0], err);
		if (inMod < 0) return sendError(err);
		int inPort = resolvePort(APP->engine->getModule(inMod), m.args[1], /*output=*/false, err);
		if (inPort < 0) return sendError(err);

		app::RackWidget* rack = APP->scene->rack;
		app::ModuleWidget* imw = rack->getModule(inMod);
		if (!imw) return sendError(string::f("no module widget %lld", (long long) inMod));
		app::PortWidget* ipw = imw->getInput(inPort);
		if (!ipw) return sendError(string::f("module %lld has no input %d", (long long) inMod, inPort));

		app::CableWidget* cw = rack->getTopCable(ipw);
		if (!cw) return sendError(string::f("no cable on input %lld:%d", (long long) inMod, inPort));
		int64_t cableId = cw->getCable() ? cw->getCable()->id : -1;

		removeCableWidget(cw);

		OscMessage out("/cable/removed");
		out.pushInt((int32_t) cableId);
		out.pushInt((int32_t) inMod);
		out.pushInt(inPort);
		reply(out);
	}

	void applyCableRemoveId(const OscMessage& m) {
		if (m.args.size() < 1) return sendError("/cable/remove_id needs <cableId>");
		int64_t cableId = m.args[0].asInt();
		app::RackWidget* rack = APP->scene->rack;
		app::CableWidget* cw = rack->getCable(cableId);
		if (!cw) return sendError(string::f("no cable %lld", (long long) cableId));

		removeCableWidget(cw);

		OscMessage out("/cable/removed");
		out.pushInt((int32_t) cableId);
		reply(out);
	}

	void removeCableWidget(app::CableWidget* cw) {
		history::CableRemove* h = new history::CableRemove;
		h->setCable(cw);
		APP->history->push(h);
		APP->scene->rack->removeCable(cw);
		delete cw; // ~CableWidget removes the engine cable
	}

	// -------------------------------------- 4c. MODULE lifecycle (app, FRAGILE!)
	// Instantiating and deleting modules also uses the unofficial app/UI layer,
	// so it lives beside the cable code and follows Rack 2.6.1's own patterns:
	//   add:    model->createModule() → createModuleWidget() → rack->addModule()
	//           (which also adds it to the engine) → position → history::ModuleAdd
	//   remove: strip every cable on the module FIRST (engine::removeModule asserts
	//           none remain) → history::ModuleRemove → rack->removeModule → delete
	// Resolve a module to *create* — it has no live instance yet, so we match
	// against the installed-module registry, symmetric with resolveModule: slug
	// (exact) OR human name / brand (case-insensitive). Ambiguity is an error
	// listing the candidate slugs (as the :N index disambiguates instances).
	plugin::Model* resolveModel(const std::string& pluginRef, const std::string& modelRef,
	                            std::string& err) {
		// Fast, canonical path: exact slugs (also handles renamed-plugin fallbacks
		// consistently with the rest of Rack).
		if (plugin::Model* m = plugin::getModel(pluginRef, modelRef))
			return m;

		std::vector<plugin::Model*> hits;
		for (plugin::Plugin* p : plugin::plugins) {
			if (!p) continue;
			if (p->slug != pluginRef && !ciEq(p->name, pluginRef)) continue;
			for (plugin::Model* model : p->models) {
				if (!model || model->hidden) continue;
				if (model->slug == modelRef || ciEq(model->name, modelRef))
					hits.push_back(model);
			}
		}
		if (hits.empty()) {
			err = "no such module: " + pluginRef + " / " + modelRef;
			return nullptr;
		}
		if (hits.size() > 1) {
			std::string cands;
			for (plugin::Model* m : hits) {
				if (!cands.empty()) cands += ", ";
				cands += (m->plugin ? m->plugin->slug : "?") + std::string("/") + m->slug;
			}
			err = "ambiguous module '" + pluginRef + "/" + modelRef + "': " + cands;
			return nullptr;
		}
		return hits[0];
	}

	void applyModuleAdd(const OscMessage& m) {
		if (m.args.size() < 2)
			return sendError("/module/add needs <plugin> <model> [x] [y]");
		if (m.args[0].type != OscArg::STRING || m.args[1].type != OscArg::STRING)
			return sendError("/module/add: plugin and model must be strings (slug or name)");

		std::string err;
		plugin::Model* model = resolveModel(m.args[0].s, m.args[1].s, err);
		if (!model)
			return sendError(err);

		engine::Module* module = model->createModule();
		if (!module) return sendError("createModule failed");
		app::ModuleWidget* mw = model->createModuleWidget(module);
		if (!mw) { delete module; return sendError("createModuleWidget failed"); }

		// rack->addModule adopts ownership and adds the module to the engine,
		// which assigns its id.
		APP->scene->rack->addModule(mw);

		if (m.args.size() >= 4) {
			math::Vec pos(m.args[2].asFloat(), m.args[3].asFloat());
			APP->scene->rack->setModulePosForce(mw, pos);
		}

		history::ModuleAdd* h = new history::ModuleAdd;
		h->setModule(mw);
		APP->history->push(h);

		int64_t id = mw->module ? mw->module->id : -1;
		OscMessage out("/module/added");
		out.pushInt((int32_t) id);
		// Echo the *canonical* slugs of the resolved model, so a client that added
		// by human name learns the stable ids.
		out.pushString(model->plugin ? model->plugin->slug : "");
		out.pushString(model->slug);
		reply(out);
	}

	void applyModuleRemove(const OscMessage& m) {
		if (m.args.size() < 1) return sendError("/module/remove needs <module>");
		std::string err;
		int64_t moduleId = resolveModule(m.args[0], err);
		if (moduleId < 0) return sendError(err);
		app::ModuleWidget* mw = APP->scene->rack->getModule(moduleId);
		if (!mw) return sendError(string::f("no module widget %lld", (long long) moduleId));

		// Engine::removeModule asserts the module has no cables — remove them first,
		// reusing the cable-removal abstraction (each with its own undo step).
		for (int64_t cid : APP->engine->getCableIds()) {
			engine::Cable* c = APP->engine->getCable(cid);
			if (!c) continue;
			bool touches = (c->outputModule && c->outputModule->id == moduleId)
			            || (c->inputModule && c->inputModule->id == moduleId);
			if (!touches) continue;
			if (app::CableWidget* cw = APP->scene->rack->getCable(cid))
				removeCableWidget(cw);
		}

		history::ModuleRemove* h = new history::ModuleRemove;
		h->setModule(mw);       // serialize state for undo while still alive
		APP->history->push(h);
		APP->scene->rack->removeModule(mw);
		delete mw;              // ~ModuleWidget removes the engine module

		OscMessage out("/module/removed");
		out.pushInt((int32_t) moduleId);
		reply(out);
	}

	// Presets are plain files on the Rack machine. NOTE: this lets an OSC client
	// read/write files by path — no worse than the arbitrary patch control the
	// module already grants, but see the README security note.
	void applyPresetSave(const OscMessage& m) {
		if (m.args.size() < 2 || m.args[1].type != OscArg::STRING)
			return sendError("/module/preset_save needs <module> <path:string>");
		std::string err;
		int64_t moduleId = resolveModule(m.args[0], err);
		if (moduleId < 0) return sendError(err);
		app::ModuleWidget* mw = APP->scene->rack->getModule(moduleId);
		if (!mw) return sendError(string::f("no module widget %lld", (long long) moduleId));
		mw->save(m.args[1].s);
		OscMessage out("/module/preset_saved");
		out.pushInt((int32_t) moduleId);
		out.pushString(m.args[1].s);
		reply(out);
	}

	void applyPresetLoad(const OscMessage& m) {
		if (m.args.size() < 2 || m.args[1].type != OscArg::STRING)
			return sendError("/module/preset_load needs <module> <path:string>");
		std::string err;
		int64_t moduleId = resolveModule(m.args[0], err);
		if (moduleId < 0) return sendError(err);
		app::ModuleWidget* mw = APP->scene->rack->getModule(moduleId);
		if (!mw) return sendError(string::f("no module widget %lld", (long long) moduleId));
		mw->loadAction(m.args[1].s);   // wraps the change in an undo action
		OscMessage out("/module/preset_loaded");
		out.pushInt((int32_t) moduleId);
		out.pushString(m.args[1].s);
		reply(out);
	}

	// ----------------------------------------------------------- 3. STATE dump
	// /state/dump [includeParams=1] [includePorts=1]
	//   → /state/module <id> <pluginSlug> <modelSlug> <numParams> <numInputs>
	//                    <numOutputs> <x> <y> <modelName> <description>
	//   → /state/param  <id> <paramId> <value> <min> <max> <label> <unit> <desc>
	//   → /state/input  <id> <portId> <name> <description>
	//   → /state/output <id> <portId> <name> <description>
	//   → /state/cable  <cableId> <outMod> <outPort> <inMod> <inPort>
	//   → /state/done   <numModules> <numCables>
	void applyStateDump(const OscMessage& m) {
		bool includeParams = m.args.size() < 1 ? true : (m.args[0].asInt() != 0);
		bool includePorts  = m.args.size() < 2 ? true : (m.args[1].asInt() != 0);

		app::RackWidget* rack = APP->scene->rack;

		// Modules
		std::vector<int64_t> moduleIds = APP->engine->getModuleIds();
		for (int64_t id : moduleIds) {
			engine::Module* mod = APP->engine->getModule(id);
			if (!mod) continue;
			plugin::Model* model = mod->getModel();
			std::string pluginSlug = (model && model->plugin) ? model->plugin->slug : "?";
			std::string modelSlug = model ? model->slug : "?";

			math::Vec pos(0, 0);
			if (app::ModuleWidget* mw = rack->getModule(id))
				pos = mw->box.pos;

			OscMessage out("/state/module");
			out.pushInt((int32_t) id);
			out.pushString(pluginSlug);
			out.pushString(modelSlug);
			out.pushInt((int32_t) mod->getNumParams());
			out.pushInt((int32_t) mod->getNumInputs());
			out.pushInt((int32_t) mod->getNumOutputs());
			out.pushFloat(pos.x);
			out.pushFloat(pos.y);
			out.pushString(model ? model->name : "");        // human-readable name
			out.pushString(model ? model->description : ""); // one-line "comment"
			reply(out);

			if (includeParams) {
				for (int p = 0; p < mod->getNumParams(); p++) {
					engine::ParamQuantity* pq = mod->getParamQuantity(p);
					OscMessage pm("/state/param");
					pm.pushInt((int32_t) id);
					pm.pushInt(p);
					pm.pushFloat(APP->engine->getParamValue(mod, p));
					pm.pushFloat(pq ? pq->getMinValue() : 0.f);
					pm.pushFloat(pq ? pq->getMaxValue() : 1.f);
					pm.pushString(pq ? pq->getLabel() : "");        // name
					pm.pushString(pq ? pq->getUnit() : "");         // e.g. " Hz", " V", "%"
					pm.pushString(pq ? pq->getDescription() : "");  // the "comment"
					reply(pm);
				}
			}

			// Named + described I/O. VCV ports carry no signal "type" (all ports
			// are ±10 V polyphonic voltage); their meaning lives in name/desc.
			if (includePorts) {
				for (int i = 0; i < mod->getNumInputs(); i++) {
					engine::PortInfo* pi = mod->getInputInfo(i);
					OscMessage im("/state/input");
					im.pushInt((int32_t) id);
					im.pushInt(i);
					im.pushString(pi ? pi->getName() : "");
					im.pushString(pi ? pi->getDescription() : "");
					reply(im);
				}
				for (int o = 0; o < mod->getNumOutputs(); o++) {
					engine::PortInfo* po = mod->getOutputInfo(o);
					OscMessage om("/state/output");
					om.pushInt((int32_t) id);
					om.pushInt(o);
					om.pushString(po ? po->getName() : "");
					om.pushString(po ? po->getDescription() : "");
					reply(om);
				}
			}
		}

		// Cables
		std::vector<int64_t> cableIds = APP->engine->getCableIds();
		for (int64_t cid : cableIds) {
			engine::Cable* c = APP->engine->getCable(cid);
			if (!c || !c->outputModule || !c->inputModule) continue;
			OscMessage out("/state/cable");
			out.pushInt((int32_t) cid);
			out.pushInt((int32_t) c->outputModule->id);
			out.pushInt(c->outputId);
			out.pushInt((int32_t) c->inputModule->id);
			out.pushInt(c->inputId);
			reply(out);
		}

		OscMessage done("/state/done");
		done.pushInt((int32_t) moduleIds.size());
		done.pushInt((int32_t) cableIds.size());
		reply(done);
	}

	// ------------------------------------------------- catalogue of installed
	// modules (module-level only — port/param names are unknown until a module
	// is instantiated, so this lists what CAN be added, not their I/O).
	// /registry/dump → /registry/model <pluginSlug> <modelSlug> <name> <desc>
	//                → /registry/done <count>
	void applyRegistryDump(const OscMessage&) {
		int count = 0;
		for (plugin::Plugin* p : plugin::plugins) {
			if (!p) continue;
			for (plugin::Model* model : p->models) {
				if (!model || model->hidden) continue;
				OscMessage out("/registry/model");
				out.pushString(p->slug);
				out.pushString(model->slug);
				out.pushString(model->name);
				out.pushString(model->description);
				reply(out);
				count++;
			}
		}
		OscMessage done("/registry/done");
		done.pushInt(count);
		reply(done);
	}

	// --------------------------------------------------------- 2. OSCQuery (HTTP)
	// Build a JSON description of the live patch's OSC namespace, per the OSCQuery
	// proposal. Each param becomes a read/write leaf at /param/<moduleId>/<paramId>
	// with its TYPE, RANGE, DESCRIPTION and current VALUE — enough for TouchOSC /
	// Open Stage Control to auto-generate a labelled, correctly-ranged surface.
	// Built on the UI thread; the string is cached for the HTTP thread.
	void rebuildOscQuery() {
		json_t* root = makeContainer("/", "OSC Controller — live VCV Rack patch");
		json_t* rootContents = json_object_get(root, "CONTENTS");

		json_t* paramC = makeContainer("/param", "Module parameters (read/write)");
		json_object_set_new(rootContents, "param", paramC);
		json_t* paramContents = json_object_get(paramC, "CONTENTS");

		for (int64_t id : APP->engine->getModuleIds()) {
			engine::Module* mod = APP->engine->getModule(id);
			if (!mod || mod->getNumParams() == 0) continue;
			plugin::Model* model = mod->getModel();
			std::string modKey = std::to_string(id);
			std::string modFull = "/param/" + modKey;
			json_t* modNode = makeContainer(modFull, model ? model->name : "");
			json_object_set_new(paramContents, modKey.c_str(), modNode);
			json_t* modContents = json_object_get(modNode, "CONTENTS");

			for (int p = 0; p < mod->getNumParams(); p++) {
				engine::ParamQuantity* pq = mod->getParamQuantity(p);
				std::string pFull = modFull + "/" + std::to_string(p);

				json_t* leaf = json_object();
				json_object_set_new(leaf, "FULL_PATH", json_string(pFull.c_str()));
				json_object_set_new(leaf, "TYPE", json_string("f"));
				json_object_set_new(leaf, "ACCESS", json_integer(3)); // read + write
				json_object_set_new(leaf, "DESCRIPTION",
					json_string(pq ? pq->getLabel().c_str() : ""));

				json_t* r0 = json_object();
				json_object_set_new(r0, "MIN", json_real(pq ? pq->getMinValue() : 0.0));
				json_object_set_new(r0, "MAX", json_real(pq ? pq->getMaxValue() : 1.0));
				json_t* range = json_array();
				json_array_append_new(range, r0);
				json_object_set_new(leaf, "RANGE", range);

				json_t* value = json_array();
				json_array_append_new(value, json_real(APP->engine->getParamValue(mod, p)));
				json_object_set_new(leaf, "VALUE", value);

				json_object_set_new(modContents, std::to_string(p).c_str(), leaf);
			}
		}

		char* dumped = json_dumps(root, JSON_COMPACT);
		{
			std::lock_guard<std::mutex> lk(oscQueryMutex);
			oscQueryDoc = dumped ? dumped : "{}";
		}
		if (dumped) free(dumped);
		json_decref(root);
	}

	static json_t* makeContainer(const std::string& fullPath, const std::string& desc) {
		json_t* n = json_object();
		json_object_set_new(n, "FULL_PATH", json_string(fullPath.c_str()));
		json_object_set_new(n, "ACCESS", json_integer(0)); // container: not settable
		if (!desc.empty())
			json_object_set_new(n, "DESCRIPTION", json_string(desc.c_str()));
		json_object_set_new(n, "CONTENTS", json_object());
		return n;
	}

	// Serve one OSCQuery HTTP request. Runs on the HTTP thread — it only reads the
	// cached doc string (under the mutex) and parses its own private copy, so it
	// never races the UI thread's json_t tree.
	std::string handleOscQuery(const std::string& path, const std::string& query) {
		// HOST_INFO: tells the client where the actual OSC (UDP) endpoint is.
		if (query.find("HOST_INFO") != std::string::npos) {
			json_t* h = json_object();
			json_object_set_new(h, "NAME", json_string("Roomi Fields OSC Controller"));
			json_object_set_new(h, "OSC_PORT", json_integer(listenPort));
			json_object_set_new(h, "OSC_TRANSPORT", json_string("UDP"));
			json_t* ext = json_object();
			const char* keys[] = {"ACCESS", "VALUE", "RANGE", "DESCRIPTION", "TYPE"};
			for (const char* k : keys)
				json_object_set_new(ext, k, json_true());
			json_object_set_new(h, "EXTENSIONS", ext);
			char* s = json_dumps(h, JSON_COMPACT);
			std::string out = s ? s : "{}";
			if (s) free(s);
			json_decref(h);
			return out;
		}

		std::string doc;
		{
			std::lock_guard<std::mutex> lk(oscQueryMutex);
			doc = oscQueryDoc;
		}
		if (doc.empty()) return "{}";

		json_error_t err;
		json_t* rootDoc = json_loads(doc.c_str(), 0, &err);
		if (!rootDoc) return "{}";

		// Navigate to the requested path via CONTENTS[segment] hops.
		json_t* node = rootDoc;
		size_t i = 0;
		while (node && i < path.size()) {
			while (i < path.size() && path[i] == '/') i++;   // skip slashes
			size_t start = i;
			while (i < path.size() && path[i] != '/') i++;
			if (i == start) break;                            // trailing slash
			std::string seg = path.substr(start, i - start);
			json_t* contents = json_object_get(node, "CONTENTS");
			node = contents ? json_object_get(contents, seg.c_str()) : nullptr;
		}

		std::string out;
		if (node) {
			char* s = json_dumps(node, JSON_COMPACT);
			out = s ? s : "{}";
			if (s) free(s);
		} else {
			out = "{\"error\":\"no such OSC path\"}";
		}
		json_decref(rootDoc);
		return out;
	}

	// ------------------------------------------------------------- persistence
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "listenPort", json_integer(listenPort));
		json_object_set_new(root, "replyHost", json_string(replyHost.c_str()));
		json_object_set_new(root, "replyPort", json_integer(replyPort));
		json_object_set_new(root, "notifyOnSet", json_boolean(notifyOnSet));
		json_object_set_new(root, "oscQueryEnabled", json_boolean(oscQueryEnabled));
		json_object_set_new(root, "oscQueryPort", json_integer(oscQueryPort));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "listenPort")) listenPort = (uint16_t) json_integer_value(j);
		if (json_t* j = json_object_get(root, "replyHost")) replyHost = json_string_value(j);
		if (json_t* j = json_object_get(root, "replyPort")) replyPort = (uint16_t) json_integer_value(j);
		if (json_t* j = json_object_get(root, "notifyOnSet")) notifyOnSet = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "oscQueryEnabled")) oscQueryEnabled = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "oscQueryPort")) oscQueryPort = (uint16_t) json_integer_value(j);
		requestRestart(); // rebind with the loaded port
	}
};

/* ==========================================================================
 * Widget — panel, activity LEDs, a tiny status display, and a config menu.
 * The crucial job: pump processPending() on the UI thread each frame.
 * ==========================================================================*/

struct OscStatusDisplay : widget::Widget {
	OscController* module = nullptr;
	std::shared_ptr<window::Font> font;

	void draw(const DrawArgs& args) override {
		if (!font)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));

		nvgScissor(args.vg, RECT_ARGS(args.clipBox));
		// Panel-inset background.
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3.0);
		nvgFillColor(args.vg, nvgRGB(0x10, 0x14, 0x12));
		nvgFill(args.vg);

		if (font && font->handle >= 0) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFillColor(args.vg, nvgRGB(0x3a, 0xff, 0x9a));
			nvgFontSize(args.vg, 11);
			nvgTextLetterSpacing(args.vg, 0.5);

			std::string l1 = module ? module->serverStatus : "listening :7770";
			std::string l2 = module ? string::f("reply %s:%d", module->replyHost.c_str(), (int) module->replyPort)
			                        : "reply 127.0.0.1:7771";
			std::string l3 = module ? module->httpStatus : "OSCQuery :7772";
			std::string l4 = module ? string::f("msgs: %d", module->messageCount.load()) : "msgs: 0";

			nvgText(args.vg, 6, 16, l1.c_str(), NULL);
			nvgText(args.vg, 6, 32, l2.c_str(), NULL);
			nvgText(args.vg, 6, 48, l3.c_str(), NULL);
			nvgText(args.vg, 6, 64, l4.c_str(), NULL);
		}
		nvgResetScissor(args.vg);
	}
};

struct OscControllerWidget : ModuleWidget {
	OscControllerWidget(OscController* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/OscController.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Status display.
		OscStatusDisplay* display = createWidget<OscStatusDisplay>(mm2px(Vec(2.5, 14.0)));
		display->box.size = mm2px(Vec(35.0, 24.0));
		display->module = module;
		addChild(display);

		// Activity LEDs (RX green, TX yellow).
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(8.0, 40.0)), module, OscController::RX_LIGHT));
		addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(20.0, 40.0)), module, OscController::TX_LIGHT));
	}

	// Pump the applicator on the UI thread. This is the hook required by the
	// threading model: cables are only ever touched from here.
	void step() override {
		if (OscController* m = dynamic_cast<OscController*>(module))
			m->processPending();
		ModuleWidget::step();
	}

	void appendContextMenu(Menu* menu) override {
		OscController* m = dynamic_cast<OscController*>(module);
		if (!m) return;

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("OSC configuration"));

		menu->addChild(createSubmenuItem("Listen port", std::to_string(m->listenPort), [m](Menu* sub) {
			for (int port : {7770, 8000, 9000, 57120}) {
				sub->addChild(createCheckMenuItem(std::to_string(port), "",
					[m, port]() { return m->listenPort == port; },
					[m, port]() { m->listenPort = (uint16_t) port; m->requestRestart(); }));
			}
		}));

		menu->addChild(createSubmenuItem("Reply port", std::to_string(m->replyPort), [m](Menu* sub) {
			for (int port : {7771, 8001, 9001, 57121}) {
				sub->addChild(createCheckMenuItem(std::to_string(port), "",
					[m, port]() { return m->replyPort == port; },
					[m, port]() { m->replyPort = (uint16_t) port; m->requestRestart(); }));
			}
		}));

		menu->addChild(createBoolPtrMenuItem("Echo /param/value on set", "", &m->notifyOnSet));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("OSCQuery (HTTP discovery)"));
		menu->addChild(createBoolMenuItem("Enable OSCQuery", "",
			[m]() { return m->oscQueryEnabled; },
			[m](bool v) { m->oscQueryEnabled = v; m->requestRestart(); }));
		menu->addChild(createSubmenuItem("OSCQuery port", std::to_string(m->oscQueryPort), [m](Menu* sub) {
			for (int port : {7772, 8080, 9000, 5678}) {
				sub->addChild(createCheckMenuItem(std::to_string(port), "",
					[m, port]() { return m->oscQueryPort == port; },
					[m, port]() { m->oscQueryPort = (uint16_t) port; m->requestRestart(); }));
			}
		}));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Restart OSC server", "", [m]() { m->requestRestart(); }));
	}
};

Model* modelOscController = createModel<OscController, OscControllerWidget>("OscController");
