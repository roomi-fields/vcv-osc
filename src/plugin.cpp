#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;

	// Register each module the plugin provides.
	p->addModel(modelOscController);
}
