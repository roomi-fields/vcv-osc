// Standalone test of the OSCQuery HTTP transport (HttpServer), independent of
// Rack. Starts the server with an echo handler, lets the shell script curl it,
// then shuts down — proving the accept loop, request parsing and clean stop().
#include "osc/HttpServer.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

int main(int argc, char** argv) {
	uint16_t port = (uint16_t) (argc > 1 ? atoi(argv[1]) : 7772);

	vcvosc::HttpServer server;
	bool ok = server.start(port, [](const std::string& path, const std::string& query) {
		// Echo the parsed request as JSON so the test can assert on it.
		std::string q = query;
		for (char& c : q) if (c == '"') c = '\'';
		return std::string("{\"path\":\"") + path + "\",\"query\":\"" + q + "\"}";
	});
	if (!ok) {
		printf("FAILED to start: %s\n", server.lastError().c_str());
		return 1;
	}
	printf("http listening :%d\n", (int) port);
	fflush(stdout);

	// Serve for a short window, then stop and confirm the thread joins cleanly.
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	server.stop();
	printf("stopped cleanly\n");
	return 0;
}
