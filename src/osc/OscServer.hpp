#pragma once
#include <thread>
#include <atomic>
#include <cstdint>
#include "UdpSocket.hpp"
#include "CommandQueue.hpp"

/** OSC receiver: owns a UDP socket + a background thread that blocks on
 * recvfrom(), parses each datagram (message or #bundle) and enqueues the
 * resulting messages into a CommandQueue for the UI thread to apply.
 *
 * Lifecycle is start()/stop(); stop() closes the socket to unblock recv() and
 * joins the thread. Safe to call stop() from the destructor. */
namespace vcvosc {

class OscServer {
public:
	explicit OscServer(CommandQueue& queue) : queue_(queue) {}
	~OscServer() { stop(); }

	/** (Re)bind to localPort and start the receive loop. Returns false if the
	 * port could not be bound (e.g. already in use). */
	bool start(uint16_t localPort);
	void stop();

	bool isRunning() const { return running_.load(); }
	uint16_t port() const { return port_; }
	const std::string& lastError() const { return lastError_; }

private:
	void run();

	CommandQueue& queue_;
	UdpSocket socket_;
	std::thread thread_;
	std::atomic<bool> running_{false};
	uint16_t port_ = 0;
	std::string lastError_;
};

} // namespace vcvosc
