#include "OscServer.hpp"

namespace vcvosc {

bool OscServer::start(uint16_t localPort) {
	stop();
	if (!socket_.open(localPort, /*bind=*/true)) {
		lastError_ = socket_.lastError();
		return false;
	}
	// Poll the stop flag every 200 ms rather than relying on close() to
	// interrupt a blocked recvfrom() (which is not portable).
	socket_.setRecvTimeout(200);
	port_ = localPort;
	running_.store(true);
	thread_ = std::thread(&OscServer::run, this);
	return true;
}

void OscServer::stop() {
	if (!running_.load() && !thread_.joinable()) {
		socket_.close();
		return;
	}
	running_.store(false);
	socket_.close(); // unblocks the recv() in run()
	if (thread_.joinable())
		thread_.join();
}

void OscServer::run() {
	// UDP datagrams are bounded; 64 KiB covers any realistic OSC packet.
	std::vector<uint8_t> buf(65536);
	while (running_.load()) {
		int n = socket_.recv(buf.data(), buf.size());
		if (n < 0) {
			// Socket closed (stop()) or transient error — exit if we're stopping.
			if (!running_.load()) break;
			continue;
		}
		if (n == 0) continue;

		std::vector<OscMessage> msgs;
		if (OscMessage::parsePacket(buf.data(), (size_t) n, msgs)) {
			for (OscMessage& m : msgs)
				queue_.push(std::move(m));
		}
		// Malformed packets are silently dropped — never crash on bad input.
	}
}

} // namespace vcvosc
