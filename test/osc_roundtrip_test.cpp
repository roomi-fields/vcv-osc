// Standalone end-to-end test of the OSC layer (no Rack needed).
// Binds OscServer on a port, drains the CommandQueue, prints every parsed
// message. Proves serialize/socket/recv/parse/queue works and is wire-compatible
// with python-osc (same OSC 1.0 encoding osc-bridge uses).
#include "src/osc/OscServer.hpp"
#include "src/osc/OscSender.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
using namespace vcvosc;

static void printMsg(const OscMessage& m) {
	printf("RX %s", m.address.c_str());
	for (const auto& a : m.args) {
		if (a.type == OscArg::INT) printf(" i:%d", a.i);
		else if (a.type == OscArg::FLOAT) printf(" f:%g", a.f);
		else printf(" s:\"%s\"", a.s.c_str());
	}
	printf("\n");
	fflush(stdout);
}

int main(int argc, char** argv) {
	uint16_t port = (argc > 1) ? (uint16_t) atoi(argv[1]) : 7770;
	CommandQueue queue;
	OscServer server(queue);
	if (!server.start(port)) {
		printf("FAIL bind :%d — %s\n", port, server.lastError().c_str());
		return 1;
	}
	printf("listening :%d\n", port);
	fflush(stdout);

	// Also self-test the serializer by looping one message back through parse().
	{
		OscMessage t("/selftest");
		t.pushInt(42).pushFloat(3.5f).pushString("hi");
		auto bytes = t.serialize();
		std::vector<OscMessage> out;
		bool ok = OscMessage::parsePacket(bytes.data(), bytes.size(), out);
		printf("selftest serialize->parse: %s (%zu msg)\n", ok ? "OK" : "FAIL", out.size());
		if (ok && !out.empty()) printMsg(out[0]);
	}

	int total = 0;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	while (std::chrono::steady_clock::now() < deadline) {
		for (auto& m : queue.drain()) { printMsg(m); total++; }
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	printf("done, %d messages received\n", total);
	server.stop();
	return 0;
}
