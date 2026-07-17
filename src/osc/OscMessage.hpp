#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

/** Minimal, dependency-free OSC 1.0 message model + (de)serialization.
 *
 * We deliberately avoid oscpack / liblo: a VCV plugin ships as a single shared
 * object, and a ~200-line self-contained codec keeps the build trivial on the
 * three platforms and sidesteps vendoring another library. We support the arg
 * types actually used on the wire by osc-bridge and python-osc: int32 ('i'),
 * float32 ('f') and string ('s'). Blobs are parsed/skipped but not produced.
 *
 * All OSC integers/floats are big-endian on the wire.
 */
namespace vcvosc {

struct OscArg {
	enum Type { INT, FLOAT, STRING } type;
	int32_t i = 0;
	float f = 0.f;
	std::string s;

	static OscArg makeInt(int32_t v)   { OscArg a; a.type = INT;   a.i = v; return a; }
	static OscArg makeFloat(float v)   { OscArg a; a.type = FLOAT; a.f = v; return a; }
	static OscArg makeString(std::string v) { OscArg a; a.type = STRING; a.s = std::move(v); return a; }

	/** Convenience readers with coercion (int<->float). */
	float asFloat() const { return type == FLOAT ? f : (type == INT ? (float) i : 0.f); }
	int32_t asInt() const { return type == INT ? i : (type == FLOAT ? (int32_t) f : 0); }
};

struct OscMessage {
	std::string address;
	std::vector<OscArg> args;

	OscMessage() {}
	explicit OscMessage(std::string addr) : address(std::move(addr)) {}

	OscMessage& pushInt(int32_t v)   { args.push_back(OscArg::makeInt(v)); return *this; }
	OscMessage& pushFloat(float v)   { args.push_back(OscArg::makeFloat(v)); return *this; }
	OscMessage& pushString(std::string v) { args.push_back(OscArg::makeString(std::move(v))); return *this; }

	// --- Serialization -----------------------------------------------------

	/** Encode this message to an OSC packet (address + typetags + args). */
	std::vector<uint8_t> serialize() const {
		std::vector<uint8_t> out;
		appendPaddedString(out, address);

		std::string tags = ",";
		for (const OscArg& a : args) {
			tags += (a.type == OscArg::INT) ? 'i' : (a.type == OscArg::FLOAT) ? 'f' : 's';
		}
		appendPaddedString(out, tags);

		for (const OscArg& a : args) {
			switch (a.type) {
				case OscArg::INT:   appendBE32(out, (uint32_t) a.i); break;
				case OscArg::FLOAT: { uint32_t u; std::memcpy(&u, &a.f, 4); appendBE32(out, u); break; }
				case OscArg::STRING: appendPaddedString(out, a.s); break;
			}
		}
		return out;
	}

	// --- Deserialization ---------------------------------------------------

	/** Parse a single OSC message from buffer. Returns false on malformed data.
	 * Does not handle bundles; see parsePacket() for that. */
	static bool parse(const uint8_t* data, size_t len, OscMessage& msg) {
		size_t pos = 0;
		std::string addr;
		if (!readPaddedString(data, len, pos, addr)) return false;
		msg.address = addr;
		msg.args.clear();

		// A message with no typetag string is technically legal (no args).
		if (pos >= len) return true;

		std::string tags;
		if (!readPaddedString(data, len, pos, tags)) return false;
		if (tags.empty() || tags[0] != ',') return false;

		for (size_t t = 1; t < tags.size(); t++) {
			char tag = tags[t];
			switch (tag) {
				case 'i': {
					uint32_t u; if (!readBE32(data, len, pos, u)) return false;
					msg.args.push_back(OscArg::makeInt((int32_t) u));
					break;
				}
				case 'f': {
					uint32_t u; if (!readBE32(data, len, pos, u)) return false;
					float fv; std::memcpy(&fv, &u, 4);
					msg.args.push_back(OscArg::makeFloat(fv));
					break;
				}
				case 's': {
					std::string sv; if (!readPaddedString(data, len, pos, sv)) return false;
					msg.args.push_back(OscArg::makeString(sv));
					break;
				}
				case 'b': {
					uint32_t sz; if (!readBE32(data, len, pos, sz)) return false;
					// Skip blob payload padded to 4 bytes; we don't surface blobs.
					size_t padded = (sz + 3) & ~3u;
					if (pos + padded > len) return false;
					pos += padded;
					break;
				}
				// Tags with no payload (T,F,N,I) carry no bytes; ignore unknowns
				// that also have no payload rather than aborting the whole message.
				case 'T': case 'F': case 'N': case 'I':
					break;
				default:
					return false;
			}
		}
		return true;
	}

	/** Parse a UDP packet that may be a plain message or a #bundle, appending
	 * every contained message to `out`. Returns false on malformed data. */
	static bool parsePacket(const uint8_t* data, size_t len, std::vector<OscMessage>& out) {
		if (len >= 8 && std::memcmp(data, "#bundle", 8) == 0) {
			// #bundle\0 (8) + timetag (8) + [ int32 size + element ]*
			size_t pos = 16;
			while (pos + 4 <= len) {
				uint32_t sz;
				if (!readBE32(data, len, pos, sz)) return false;
				if (pos + sz > len) return false;
				// Recurse: bundle elements can be messages or nested bundles.
				if (!parsePacket(data + pos, sz, out)) return false;
				pos += sz;
			}
			return true;
		}
		OscMessage m;
		if (!parse(data, len, m)) return false;
		out.push_back(std::move(m));
		return true;
	}

private:
	static void appendPaddedString(std::vector<uint8_t>& out, const std::string& s) {
		out.insert(out.end(), s.begin(), s.end());
		out.push_back(0);
		while (out.size() % 4 != 0) out.push_back(0);
	}
	static void appendBE32(std::vector<uint8_t>& out, uint32_t v) {
		out.push_back((v >> 24) & 0xff);
		out.push_back((v >> 16) & 0xff);
		out.push_back((v >> 8) & 0xff);
		out.push_back(v & 0xff);
	}
	static bool readPaddedString(const uint8_t* data, size_t len, size_t& pos, std::string& out) {
		size_t start = pos;
		while (pos < len && data[pos] != 0) pos++;
		if (pos >= len) return false; // no null terminator
		out.assign((const char*) data + start, pos - start);
		pos++; // consume null
		while (pos % 4 != 0) { if (pos > len) return false; pos++; }
		return true;
	}
	static bool readBE32(const uint8_t* data, size_t len, size_t& pos, uint32_t& out) {
		if (pos + 4 > len) return false;
		out = ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16)
		    | ((uint32_t) data[pos + 2] << 8) | ((uint32_t) data[pos + 3]);
		pos += 4;
		return true;
	}
};

} // namespace vcvosc
