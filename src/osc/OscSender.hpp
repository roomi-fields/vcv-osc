#pragma once
#include <string>
#include <mutex>
#include "UdpSocket.hpp"
#include "OscMessage.hpp"

/** OSC sender: a send-only UDP socket targeting one host:port (by default the
 * osc-bridge reply port). Used for /param/get replies, /state/dump output and
 * bidirectional change notifications. Sends are serialized by a mutex because
 * they may originate from the UI thread (query replies) and, potentially, a
 * notification path. */
namespace vcvosc {

class OscSender {
public:
	OscSender() {}

	/** Set the destination. Opens the socket lazily on first send. */
	void setTarget(const std::string& host, uint16_t port) {
		std::lock_guard<std::mutex> lock(mutex_);
		host_ = host;
		port_ = port;
	}

	std::string host() { std::lock_guard<std::mutex> l(mutex_); return host_; }
	uint16_t port() { std::lock_guard<std::mutex> l(mutex_); return port_; }

	/** Serialize and send one OSC message. Returns false on socket error. */
	bool send(const OscMessage& msg) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!socket_.isOpen()) {
			if (!socket_.open(0, /*bind=*/false))
				return false;
		}
		std::vector<uint8_t> packet = msg.serialize();
		return socket_.sendTo(host_, port_, packet.data(), packet.size()) >= 0;
	}

private:
	std::mutex mutex_;
	UdpSocket socket_;
	std::string host_ = "127.0.0.1";
	uint16_t port_ = 7771; // osc-bridge reply_port
};

} // namespace vcvosc
