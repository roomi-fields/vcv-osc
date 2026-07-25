#include "HttpServer.hpp"
#include <cstring>
#include <vector>

#if defined(_WIN32)
	#include <winsock2.h>
	#include <ws2tcpip.h>
	typedef int socklen_t;
	#define closesocket_ closesocket
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <unistd.h>
	#include <sys/select.h>
	#include <errno.h>
	#define closesocket_ ::close
#endif

namespace vcvosc {

#if defined(_WIN32)
// Winsock is already reference-counted by UdpSocket; but the HTTP server may be
// created independently, so guard here too with its own counter. WSAStartup is
// idempotent-ish (refcounted by the OS), and pairing Startup/Cleanup per user is
// the documented contract.
static int g_httpWsa = 0;
static void httpWsaAcquire() {
	if (g_httpWsa++ == 0) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
}
static void httpWsaRelease() {
	if (--g_httpWsa == 0) WSACleanup();
}
#endif

bool HttpServer::start(uint16_t port, Handler handler) {
	stop();
#if defined(_WIN32)
	httpWsaAcquire();
#endif
	handler_ = std::move(handler);

	int fd = (int) ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		lastError_ = "socket() failed";
#if defined(_WIN32)
		httpWsaRelease();
#endif
		return false;
	}

	int yes = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*) &yes, sizeof(yes));

	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (::bind(fd, (sockaddr*) &addr, sizeof(addr)) != 0) {
		lastError_ = "bind() failed (port in use?)";
		closesocket_(fd);
#if defined(_WIN32)
		httpWsaRelease();
#endif
		return false;
	}
	if (::listen(fd, 4) != 0) {
		lastError_ = "listen() failed";
		closesocket_(fd);
#if defined(_WIN32)
		httpWsaRelease();
#endif
		return false;
	}

	listenFd_ = fd;
	running_.store(true);
	lastError_.clear();
	thread_ = std::thread(&HttpServer::run, this);
	return true;
}

void HttpServer::stop() {
	running_.store(false);
	if (listenFd_ >= 0) {
		closesocket_(listenFd_);
		listenFd_ = -1;
	}
	if (thread_.joinable())
		thread_.join();
#if defined(_WIN32)
	if (handler_) { httpWsaRelease(); handler_ = nullptr; }
#endif
}

// Read a full HTTP request header block (until "\r\n\r\n") or give up.
static bool readRequest(int fd, std::string& out) {
	char buf[4096];
	out.clear();
	for (int i = 0; i < 8; i++) {
		int n = (int) ::recv(fd, buf, sizeof(buf), 0);
		if (n <= 0) return !out.empty();
		out.append(buf, n);
		if (out.find("\r\n\r\n") != std::string::npos) return true;
		if (out.size() > 16384) return true; // header too large; stop
	}
	return !out.empty();
}

// Minimal percent-decode for request targets (OSCQuery paths are plain ASCII,
// but be defensive).
static std::string urlDecode(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size()) {
			auto hex = [](char c) -> int {
				if (c >= '0' && c <= '9') return c - '0';
				if (c >= 'a' && c <= 'f') return c - 'a' + 10;
				if (c >= 'A' && c <= 'F') return c - 'A' + 10;
				return -1;
			};
			int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
			if (hi >= 0 && lo >= 0) { out.push_back((char) (hi * 16 + lo)); i += 2; continue; }
		}
		out.push_back(s[i] == '+' ? ' ' : s[i]);
	}
	return out;
}

void HttpServer::run() {
	while (running_.load()) {
		// Wait for a connection with a timeout so we can poll the stop flag.
		fd_set rs;
		FD_ZERO(&rs);
		FD_SET(listenFd_, &rs);
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 200 * 1000;
		int sel = ::select(listenFd_ + 1, &rs, nullptr, nullptr, &tv);
		if (sel <= 0) continue;
		if (!running_.load()) break;

		int client = (int) ::accept(listenFd_, nullptr, nullptr);
		if (client < 0) continue;

		// Bound the client's slowness.
#if defined(_WIN32)
		DWORD to = 1000;
		::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*) &to, sizeof(to));
#else
		timeval cto; cto.tv_sec = 1; cto.tv_usec = 0;
		::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &cto, sizeof(cto));
#endif

		std::string req;
		std::string body = "{}";
		int status = 200;
		const char* statusText = "OK";
		if (readRequest(client, req)) {
			// Parse the request line: "GET /target HTTP/1.1".
			std::string method, target;
			{
				size_t sp1 = req.find(' ');
				size_t sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
				if (sp1 != std::string::npos && sp2 != std::string::npos) {
					method = req.substr(0, sp1);
					target = req.substr(sp1 + 1, sp2 - sp1 - 1);
				}
			}
			if (method != "GET") {
				status = 405; statusText = "Method Not Allowed";
				body = "{\"error\":\"only GET is supported\"}";
			} else {
				std::string path = target, query;
				size_t q = target.find('?');
				if (q != std::string::npos) {
					path = target.substr(0, q);
					query = target.substr(q + 1);
				}
				body = handler_ ? handler_(urlDecode(path), query) : std::string("{}");
			}
		}

		std::string resp = "HTTP/1.1 ";
		resp += std::to_string(status);
		resp += ' ';
		resp += statusText;
		resp += "\r\nContent-Type: application/json\r\n";
		resp += "Access-Control-Allow-Origin: *\r\n"; // allow web/OSCQuery clients
		resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
		resp += "Connection: close\r\n\r\n";
		resp += body;

		size_t sent = 0;
		while (sent < resp.size()) {
			int n = (int) ::send(client, resp.data() + sent, (int) (resp.size() - sent), 0);
			if (n <= 0) break;
			sent += n;
		}
		closesocket_(client);
	}
}

} // namespace vcvosc
