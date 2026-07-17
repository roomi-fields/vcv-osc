#pragma once
#include <string>
#include <cstdint>
#include <vector>

/** Thin cross-platform UDP socket wrapper (POSIX BSD sockets / Winsock2).
 *
 * VCV plugins run on Windows, macOS and Linux from one codebase, so the only
 * platform-specific code in this plugin is confined here. We branch on the
 * compiler's own _WIN32 macro (not Rack's ARCH_WIN, which is a Make variable)
 * so this layer stays Rack-independent and unit-testable. On Windows we lazily
 * WSAStartup() once per process. */
namespace vcvosc {

class UdpSocket {
public:
	UdpSocket();
	~UdpSocket();

	UdpSocket(const UdpSocket&) = delete;
	UdpSocket& operator=(const UdpSocket&) = delete;

	/** Open a socket bound to the given local port (for receiving).
	 * Pass port 0 and bind=false for a send-only socket. Returns false on error. */
	bool open(uint16_t localPort, bool bind);
	void close();

	/** Set a receive timeout (ms) so a blocking recv() wakes periodically.
	 * Essential for clean shutdown: closing a socket does NOT reliably unblock
	 * a thread parked in recvfrom() on Linux, so the receive loop instead polls
	 * its stop flag on each timeout. Returns false on error. */
	bool setRecvTimeout(int milliseconds);
	bool isOpen() const { return fd_ >= 0; }

	/** Blocking receive of one datagram. Returns bytes read, or -1 on error/close. */
	int recv(uint8_t* buf, size_t maxLen);

	/** Send a datagram to host:port. Returns bytes sent, or -1 on error. */
	int sendTo(const std::string& host, uint16_t port, const uint8_t* data, size_t len);

	/** Last error string (best-effort, for logging). */
	const std::string& lastError() const { return lastError_; }

private:
	int fd_ = -1;             // Winsock SOCKET is stored here too (cast).
	std::string lastError_;
	void setError(const std::string& where);
};

} // namespace vcvosc
