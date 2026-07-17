#include "plugin.hpp"
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>

#include "osc/OscMessage.hpp"
#include "osc/CommandQueue.hpp"
#include "osc/OscServer.hpp"
#include "osc/OscSender.hpp"

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

	// --- Runtime ---
	CommandQueue queue;
	std::unique_ptr<OscServer> server;
	OscSender sender;

	std::atomic<bool> configDirty{true};   // (re)start server on next UI step
	std::atomic<bool> rxActivity{false};
	std::atomic<bool> txActivity{false};
	std::atomic<int> messageCount{0};
	bool serverOk = false;
	std::string serverStatus = "starting…";

	// Bonus: parameter change watches for bidirectional control.
	struct Watch { int64_t moduleId; int paramId; float last; };
	std::vector<Watch> watches;

	OscController() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		server = std::unique_ptr<OscServer>(new OscServer(queue));
	}

	~OscController() override {
		if (server)
			server->stop();
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
	}

	void requestRestart() { configDirty.store(true); }

	void restartServer() {
		serverOk = server->start(listenPort);
		serverStatus = serverOk ? string::f("listening :%d", (int) listenPort)
		                        : ("error: " + server->lastError());
		sender.setTarget(replyHost, replyPort);
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
		else if (addr == "/state/dump")  applyStateDump(msg);
		else if (addr == "/ping")        reply(OscMessage("/pong"));
		else sendError("unknown address: " + addr);
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

	// -------------------------------------------------------- 4a. PARAM access
	// Engine API is public + stable. Safe from any thread, but we stay on the UI
	// thread for consistent ordering with cable ops.
	void applyParamSet(const OscMessage& m) {
		if (m.args.size() < 3) return sendError("/param/set needs <moduleId> <paramId> <value>");
		int64_t moduleId = m.args[0].asInt();
		int paramId = m.args[1].asInt();
		float value = m.args[2].asFloat();

		engine::Module* mod = APP->engine->getModule(moduleId);
		if (!mod) return sendError(string::f("no module %lld", (long long) moduleId));
		if (paramId < 0 || paramId >= mod->getNumParams())
			return sendError(string::f("module %lld has no param %d", (long long) moduleId, paramId));

		// Clamp to the param's declared range when it has a ParamQuantity.
		if (engine::ParamQuantity* pq = mod->getParamQuantity(paramId))
			value = clamp(value, pq->getMinValue(), pq->getMaxValue());

		APP->engine->setParamValue(mod, paramId, value);

		if (notifyOnSet)
			emitParamValue(moduleId, paramId);
	}

	void applyParamGet(const OscMessage& m) {
		if (m.args.size() < 2) return sendError("/param/get needs <moduleId> <paramId>");
		int64_t moduleId = m.args[0].asInt();
		int paramId = m.args[1].asInt();
		engine::Module* mod = APP->engine->getModule(moduleId);
		if (!mod) return sendError(string::f("no module %lld", (long long) moduleId));
		if (paramId < 0 || paramId >= mod->getNumParams())
			return sendError(string::f("module %lld has no param %d", (long long) moduleId, paramId));
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
		if (m.args.size() < 2) return sendError("/param/watch needs <moduleId> <paramId> [on=1]");
		int64_t moduleId = m.args[0].asInt();
		int paramId = m.args[1].asInt();
		bool on = m.args.size() >= 3 ? (m.args[2].asInt() != 0) : true;

		auto it = std::find_if(watches.begin(), watches.end(), [&](const Watch& w) {
			return w.moduleId == moduleId && w.paramId == paramId;
		});
		if (on) {
			if (it == watches.end()) {
				float cur = 0.f;
				if (engine::Module* mod = APP->engine->getModule(moduleId))
					if (paramId >= 0 && paramId < mod->getNumParams())
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
			return sendError("/cable/add needs <outModuleId> <outPortId> <inModuleId> <inPortId>");
		int64_t outMod = m.args[0].asInt();
		int outPort = m.args[1].asInt();
		int64_t inMod = m.args[2].asInt();
		int inPort = m.args[3].asInt();

		app::RackWidget* rack = APP->scene->rack;
		app::ModuleWidget* omw = rack->getModule(outMod);
		app::ModuleWidget* imw = rack->getModule(inMod);
		if (!omw) return sendError(string::f("no module %lld", (long long) outMod));
		if (!imw) return sendError(string::f("no module %lld", (long long) inMod));

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
			return sendError("/cable/remove needs <inModuleId> <inPortId>");
		int64_t inMod = m.args[0].asInt();
		int inPort = m.args[1].asInt();

		app::RackWidget* rack = APP->scene->rack;
		app::ModuleWidget* imw = rack->getModule(inMod);
		if (!imw) return sendError(string::f("no module %lld", (long long) inMod));
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

	// ----------------------------------------------------------- 3. STATE dump
	void applyStateDump(const OscMessage& m) {
		bool includeParams = m.args.empty() ? true : (m.args[0].asInt() != 0);

		app::RackWidget* rack = APP->scene->rack;

		// Modules
		std::vector<int64_t> moduleIds = APP->engine->getModuleIds();
		for (int64_t id : moduleIds) {
			engine::Module* mod = APP->engine->getModule(id);
			if (!mod) continue;
			std::string pluginSlug = mod->getModel() && mod->getModel()->plugin
				? mod->getModel()->plugin->slug : "?";
			std::string modelSlug = mod->getModel() ? mod->getModel()->slug : "?";

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
					pm.pushString(pq ? pq->getLabel() : "");
					reply(pm);
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

	// ------------------------------------------------------------- persistence
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "listenPort", json_integer(listenPort));
		json_object_set_new(root, "replyHost", json_string(replyHost.c_str()));
		json_object_set_new(root, "replyPort", json_integer(replyPort));
		json_object_set_new(root, "notifyOnSet", json_boolean(notifyOnSet));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "listenPort")) listenPort = (uint16_t) json_integer_value(j);
		if (json_t* j = json_object_get(root, "replyHost")) replyHost = json_string_value(j);
		if (json_t* j = json_object_get(root, "replyPort")) replyPort = (uint16_t) json_integer_value(j);
		if (json_t* j = json_object_get(root, "notifyOnSet")) notifyOnSet = json_boolean_value(j);
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
			std::string l3 = module ? string::f("msgs: %d", module->messageCount.load()) : "msgs: 0";

			nvgText(args.vg, 6, 16, l1.c_str(), NULL);
			nvgText(args.vg, 6, 32, l2.c_str(), NULL);
			nvgText(args.vg, 6, 48, l3.c_str(), NULL);
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
		display->box.size = mm2px(Vec(35.0, 20.0));
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
		menu->addChild(createMenuItem("Restart OSC server", "", [m]() { m->requestRestart(); }));
	}
};

Model* modelOscController = createModel<OscController, OscControllerWidget>("OscController");
