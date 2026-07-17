#include "UdpSocket.hpp"
#include <cstring>

#if defined(_WIN32)
	#include <winsock2.h>
	#include <ws2tcpip.h>
	typedef int socklen_t;
	#define VCVOSC_INVALID (-1)
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <netdb.h>
	#include <unistd.h>
	#include <errno.h>
	#define VCVOSC_INVALID (-1)
	#define closesocket ::close
#endif

namespace vcvosc {

#if defined(_WIN32)
// Reference-counted Winsock init so multiple module instances are safe.
static int g_wsaCount = 0;
static void wsaAcquire() {
	if (g_wsaCount++ == 0) {
		WSADATA d;
		WSAStartup(MAKEWORD(2, 2), &d);
	}
}
static void wsaRelease() {
	if (--g_wsaCount == 0)
		WSACleanup();
}
#endif

UdpSocket::UdpSocket() {
#if defined(_WIN32)
	wsaAcquire();
#endif
}

UdpSocket::~UdpSocket() {
	close();
#if defined(_WIN32)
	wsaRelease();
#endif
}

void UdpSocket::setError(const std::string& where) {
	lastError_ = where;
}

bool UdpSocket::open(uint16_t localPort, bool bind) {
	close();
	int fd = (int) ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd == VCVOSC_INVALID) { setError("socket() failed"); return false; }

	// Allow quick rebind after restart.
	int yes = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*) &yes, sizeof(yes));

	if (bind) {
		sockaddr_in addr;
		std::memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(localPort);
		if (::bind(fd, (sockaddr*) &addr, sizeof(addr)) != 0) {
			setError("bind() failed (port in use?)");
			closesocket(fd);
			return false;
		}
	}
	fd_ = fd;
	lastError_.clear();
	return true;
}

bool UdpSocket::setRecvTimeout(int milliseconds) {
	if (fd_ < 0) return false;
#if defined(_WIN32)
	DWORD tv = (DWORD) milliseconds;
	return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tv, sizeof(tv)) == 0;
#else
	struct timeval tv;
	tv.tv_sec = milliseconds / 1000;
	tv.tv_usec = (milliseconds % 1000) * 1000;
	return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

void UdpSocket::close() {
	if (fd_ >= 0) {
		closesocket(fd_);
		fd_ = -1;
	}
}

int UdpSocket::recv(uint8_t* buf, size_t maxLen) {
	if (fd_ < 0) return -1;
	sockaddr_in from;
	socklen_t fromLen = sizeof(from);
	int n = (int) ::recvfrom(fd_, (char*) buf, (int) maxLen, 0, (sockaddr*) &from, &fromLen);
	return n;
}

int UdpSocket::sendTo(const std::string& host, uint16_t port, const uint8_t* data, size_t len) {
	if (fd_ < 0) return -1;
	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	// Numeric host (e.g. 127.0.0.1) is the expected case; fall back to resolve.
	if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
		addrinfo hints, *res = nullptr;
		std::memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
			setError("resolve failed");
			return -1;
		}
		addr.sin_addr = ((sockaddr_in*) res->ai_addr)->sin_addr;
		::freeaddrinfo(res);
	}
	int n = (int) ::sendto(fd_, (const char*) data, (int) len, 0, (sockaddr*) &addr, sizeof(addr));
	return n;
}

} // namespace vcvosc
