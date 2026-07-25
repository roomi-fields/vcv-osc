#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>

namespace vcvosc {

/** Minimal single-threaded HTTP/1.1 server, used to serve the OSCQuery
 * namespace description (see OscController::buildOscQuery()).
 *
 * OSCQuery (https://github.com/Vidvox/OSCQueryProposal) layers a small HTTP/JSON
 * discovery surface on top of an OSC device: a controller (TouchOSC, Open Stage
 * Control, Vezér, …) GETs the namespace tree and auto-builds a control surface
 * with the right addresses, ranges and names. We only need GET.
 *
 * Cross-platform (Winsock / BSD sockets), mirroring UdpSocket. The accept loop
 * uses select() with a timeout so stop() unblocks it promptly (same rationale
 * as OscServer's recv timeout).
 */
class HttpServer {
public:
	// Returns the JSON body for a GET. `path` is the request target before '?'
	// (percent-decoded); `query` is the raw query string after '?' (may be empty,
	// e.g. "HOST_INFO"). Called on the HTTP thread — must be thread-safe.
	using Handler = std::function<std::string(const std::string& path, const std::string& query)>;

	HttpServer() {}
	~HttpServer() { stop(); }

	bool start(uint16_t port, Handler handler);
	void stop();
	bool ok() const { return running_.load(); }
	std::string lastError() const { return lastError_; }

private:
	void run();

	int listenFd_ = -1;
	std::thread thread_;
	std::atomic<bool> running_{false};
	Handler handler_;
	std::string lastError_;
};

} // namespace vcvosc
