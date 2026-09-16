// ============================================================================
//  src/http_socket.cpp — implementation of the UNIX-socket HTTP layer
// ============================================================================

#include "Chaos_Forger/http_socket.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

#include "Chaos_Forger/log.hpp"

namespace Chaos_Forger {

std::string sys_error(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

// ============================================================================
// UnixSocketTransport
// ============================================================================

UnixSocketTransport::UnixSocketTransport(std::string socket_path, int timeout_sec)
    : socket_path_(std::move(socket_path)), timeout_sec_(timeout_sec) {}

UnixSocketTransport::~UnixSocketTransport() { close(); }

bool UnixSocketTransport::connect(std::string& err) {
    if (socket_path_.empty()) {
        err = "socket path is empty";
        return false;
    }
    if (socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
        err = "socket path too long: '" + socket_path_ + "'";
        return false;
    }
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        err = sys_error("socket(AF_UNIX) failed");
        return false;
    }
    struct timeval tv{};
    tv.tv_sec = timeout_sec_;
    tv.tv_usec = 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = sys_error("connect('" + socket_path_ + "') failed");
        close();
        return false;
    }
    return true;
}

bool UnixSocketTransport::send_all(const char* data, size_t len, std::string& err) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = sys_error("send() failed");
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool UnixSocketTransport::read_some(char* buf, size_t buf_len, size_t* len, bool* got_eof,
                                    std::string& err) {
    *len = 0;
    *got_eof = false;
    while (true) {
        const ssize_t n = ::recv(fd_, buf, buf_len, 0);
        if (n > 0) {
            *len = static_cast<size_t>(n);
            return true;
        }
        if (n == 0) {
            *got_eof = true;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            err = sys_error("recv() timed out");
            return false;
        }
        err = sys_error("recv() failed");
        return false;
    }
}

void UnixSocketTransport::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::string UnixSocketTransport::describe() const { return "unix:" + socket_path_; }

// ============================================================================
// HttpResponseParser
// ============================================================================

HttpResponseParser::HttpResponseParser(bool allow_eof_termination)
    : HttpResponseParser(allow_eof_termination, Limits()) {}

HttpResponseParser::HttpResponseParser(bool allow_eof_termination, const Limits& limits)
    : limits_(limits), allow_eof_termination_(allow_eof_termination) {}

bool HttpResponseParser::fail(const std::string& msg) {
    if (state_ != State::Error) {
        state_ = State::Error;
        error_ = msg;
    }
    return false;
}

bool HttpResponseParser::feed(const char* data, size_t len) {
    if (state_ == State::Done) return true;
    if (state_ == State::Error) return false;

    // EOF marker: feed(nullptr, 0). Finalizes EOF-framed bodies; any other
    // state means the connection died mid-response -> malformed.
    if (data == nullptr && len == 0) {
        if (state_ == State::BodyEof && allow_eof_termination_) {
            resp_.body = std::move(buf_);
            buf_.clear();
            state_ = State::Done;
            return true;
        }
        return fail("connection closed before response was complete");
    }

    buf_.append(data, len);

    // Header-phase guard: unbounded headers are a protocol DoS vector.
    if ((state_ == State::StatusLine || state_ == State::Headers) &&
        buf_.size() > limits_.max_header_bytes) {
        return fail("header block exceeds " + std::to_string(limits_.max_header_bytes) + " bytes");
    }
    if (state_ == State::BodyEof && buf_.size() > limits_.max_body_bytes) {
        return fail("EOF-framed body exceeds limit");
    }
    return step();
}

bool HttpResponseParser::step() {
    switch (state_) {
        case State::StatusLine:
        case State::Headers: {
            const size_t header_end = buf_.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                return true;  // need more bytes
            }
            if (!parse_status_line(header_end)) return false;
            if (!parse_headers(header_end)) return false;

            // Enter the body phase with the bytes left after the header block.
            buf_.erase(0, header_end + 4);

            switch (framing_) {
                case Framing::ContentLength:
                    state_ = State::BodyContentLength;
                    break;
                case Framing::Chunked:
                    state_ = State::BodyChunked;
                    break;
                case Framing::None:
                case Framing::Eof:
                    if (!allow_eof_termination_) {
                        return fail(
                            "response has neither Content-Length nor chunked framing");
                    }
                    state_ = State::BodyEof;
                    break;
            }
            return step();  // handle any body bytes already in buf_
        }

        case State::BodyContentLength:
            return body_step_content_length();
        case State::BodyChunked:
            return body_step_chunked();
        case State::BodyEof:
            return true;  // all buffered bytes are body; finish on feed(EOF)
        case State::Done:
            return true;
        case State::Error:
            return false;
    }
    return fail("parser in unknown state");
}

// Strict status-line parsing: HTTP/1.x <SP> <3-digit> <SP> reason.
// Rejects "HTTP/1.1 20x OK" style garbage that lenient atoi() would accept.
bool HttpResponseParser::parse_status_line(size_t header_end) {
    (void)header_end;
    const size_t first_line_end = buf_.find("\r\n");
    const std::string line = buf_.substr(0, first_line_end);

    static const char kScheme[] = "HTTP/";
    if (line.compare(0, 5, kScheme) != 0) {
        return fail("malformed status line (missing HTTP/ prefix): '" + line.substr(0, 64) + "'");
    }
    size_t p = 5;
    if (line.compare(p, 4, "1.0 ") != 0 && line.compare(p, 4, "1.1 ") != 0) {
        return fail("unsupported HTTP version in status line: '" + line.substr(0, 64) + "'");
    }
    p += 4;
    if (p + 3 > line.size() ||
        !std::isdigit(static_cast<unsigned char>(line[p])) ||
        !std::isdigit(static_cast<unsigned char>(line[p + 1])) ||
        !std::isdigit(static_cast<unsigned char>(line[p + 2]))) {
        return fail("malformed status code in status line: '" + line.substr(0, 64) + "'");
    }
    resp_.status = (line[p] - '0') * 100 + (line[p + 1] - '0') * 10 + (line[p + 2] - '0');
    p += 3;
    if (p < line.size() && line[p] != ' ') {
        return fail("malformed status line: '" + line.substr(0, 64) + "'");
    }
    if (p < line.size()) ++p;  // skip the single SP before the reason phrase
    resp_.status_text = line.substr(p);
    return true;
}

bool HttpResponseParser::parse_headers(size_t header_end) {
    // Headers are strictly the bytes between the status line and the blank
    // line -- never body bytes. Parsing beyond header_end would let a body
    // starting with "Field: value" hijack the framing.
    const size_t first_line_end = buf_.find("\r\n");
    const std::string headers =
        buf_.substr(first_line_end + 2, header_end - (first_line_end + 2));
    framing_ = Framing::None;

    size_t pos = 0;
    while (pos < headers.size()) {
        size_t eol = headers.find("\r\n", pos);
        if (eol == std::string::npos) eol = headers.size();
        const std::string line = headers.substr(pos, eol - pos);
        pos = eol + 2;

        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;  // tolerate junk lines

        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // trim OWS
        const auto is_ows = [](char c) { return c == ' ' || c == '\t'; };
        while (!value.empty() && is_ows(value.front())) value.erase(value.begin());
        while (!value.empty() && is_ows(value.back())) value.pop_back();

        // Case-insensitive field-name compare.
        const auto ieq = [](const std::string& a, const char* b) {
            size_t blen = std::strlen(b);
            if (a.size() != blen) return false;
            for (size_t i = 0; i < blen; ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        };

        if (ieq(name, "content-length")) {
            if (framing_ == Framing::Chunked) continue;  // chunked wins per RFC 7230
            char* endp = nullptr;
            const unsigned long long v = std::strtoull(value.c_str(), &endp, 10);
            if (endp == value.c_str() || *endp != '\0') {
                return fail("malformed Content-Length value: '" + value.substr(0, 64) + "'");
            }
            if (v > limits_.max_body_bytes) {
                return fail("Content-Length " + std::to_string(v) + " exceeds limit");
            }
            framing_ = Framing::ContentLength;
            content_length_ = static_cast<size_t>(v);
        } else if (ieq(name, "transfer-encoding")) {
            if (value.find("chunked") != std::string::npos) {
                framing_ = Framing::Chunked;  // overrides Content-Length per RFC 7230 3.3.3
            }
        }
        // Other headers are irrelevant to this client and intentionally ignored.
    }
    return true;
}

bool HttpResponseParser::body_step_content_length() {
    const size_t needed = content_length_;
    if (buf_.size() < needed) return true;  // need more bytes
    resp_.body.assign(buf_, 0, needed);
    buf_.erase(0, needed);

    if (buf_.empty()) {
        state_ = State::Done;
        return true;
    }
    return fail("unexpected extra bytes after Content-Length body (" +
                std::to_string(buf_.size()) + ")");
}

bool HttpResponseParser::body_step_chunked() {
    while (true) {
        if (!chunk_started_) {
            // Read the chunk-size line: "<hex>[;extensions]\r\n"
            const size_t eol = buf_.find("\r\n");
            if (eol == std::string::npos) {
                if (buf_.size() > limits_.max_chunk_line_bytes) {
                    return fail("chunk-size line exceeds limit");
                }
                return true;  // need more bytes
            }
            std::string size_line = buf_.substr(0, eol);
            buf_.erase(0, eol + 2);

            // Reject sizes with leading sign/space junk: parse strictly hex.
            size_t hex_end = size_line.find(';');
            if (hex_end == std::string::npos) hex_end = size_line.size();
            const std::string hex = size_line.substr(0, hex_end);
            if (hex.empty()) return fail("empty chunk-size line");
            for (const char c : hex) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) {
                    return fail("malformed chunk size: '" + size_line.substr(0, 64) + "'");
                }
            }
            // uint64 holds at most 16 hex digits; anything longer would wrap
            // strtoull to a tiny (or 0) size and desynchronize the stream.
            if (hex.size() > 16) {
                return fail("chunk size too long: '" + size_line.substr(0, 64) + "'");
            }
            const unsigned long long size = std::strtoull(hex.c_str(), nullptr, 16);

            if (size == 0) {
                // Last chunk: consume trailers until the blank line. The total
                // trailer budget is bounded, so a hostile stream cannot keep
                // the parser alive with an endless trickle of trailer lines.
                chunk_trailer_done_ = false;
                trailer_bytes_ = 0;
                size_t tpos = 0;
                while (true) {
                    const size_t teol = buf_.find("\r\n", tpos);
                    if (teol == std::string::npos) {
                        if (buf_.size() > limits_.max_chunk_line_bytes) {
                            return fail("trailer section exceeds limit");
                        }
                        return true;  // need more bytes
                    }
                    trailer_bytes_ += teol + 2 - tpos;
                    if (trailer_bytes_ > limits_.max_chunk_line_bytes) {
                        return fail("trailer section exceeds limit");
                    }
                    const std::string tline = buf_.substr(tpos, teol - tpos);
                    tpos = teol + 2;
                    if (tline.empty()) {  // blank line terminates trailers
                        chunk_trailer_done_ = true;
                        break;
                    }
                }
                buf_.erase(0, tpos);
                if (chunk_trailer_done_) {
                    state_ = State::Done;
                    return true;
                }
            }

            if (size > limits_.max_body_bytes || resp_.body.size() + size > limits_.max_body_bytes) {
                return fail("chunked body exceeds limit");
            }
            chunk_remaining_ = static_cast<size_t>(size);
            chunk_started_ = true;
        }

        if (chunk_remaining_ > 0) {
            if (buf_.empty()) return true;  // need more bytes
            const size_t take = std::min(chunk_remaining_, buf_.size());
            resp_.body.append(buf_, 0, take);
            buf_.erase(0, take);
            chunk_remaining_ -= take;
            if (chunk_remaining_ > 0) return true;  // need more bytes
        }

        // Chunk payload complete: expect the terminating CRLF.
        if (buf_.size() < 2) return true;  // need more bytes
        if (buf_.compare(0, 2, "\r\n") != 0) {
            return fail("chunk data not terminated by CRLF");
        }
        buf_.erase(0, 2);
        chunk_started_ = false;  // next chunk
    }
}

// ============================================================================
// HttpClient
// ============================================================================

std::string build_http_request(const std::string& method, const std::string& path_with_query,
                               const std::string& body) {
    std::ostringstream req;
    req << method << " " << path_with_query << " HTTP/1.1\r\n"
        << "Host: docker\r\n"
        << "User-Agent: Chaos_Forger/1.0\r\n"
        << "Accept: application/json\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "\r\n"
        << body;
    return req.str();
}

HttpClient::HttpClient(std::unique_ptr<Transport> transport)
    : transport_(std::move(transport)) {}

bool HttpClient::request(const std::string& method, const std::string& path_with_query,
                         const std::string& body, HttpResponse& out, std::string& err) {
    if (!transport_->connect(err)) {
        err = "connect to " + transport_->describe() + " failed: " + err;
        return false;
    }

    const std::string wire = build_http_request(method, path_with_query, body);
    if (!transport_->send_all(wire.data(), wire.size(), err)) {
        transport_->close();
        err = "send failed: " + err;
        return false;
    }

    HttpResponseParser parser(true /* Connection: close => EOF termination allowed */);
    char buf[8192];
    while (!parser.done()) {
        size_t n = 0;
        bool eof = false;
        if (!transport_->read_some(buf, sizeof(buf), &n, &eof, err)) {
            transport_->close();
            err = "receive failed: " + err;
            return false;
        }
        if (eof) {
            if (!parser.feed(nullptr, 0)) {  // EOF marker
                transport_->close();
                err = "malformed response: " + parser.error();
                return false;
            }
            break;
        }
        if (!parser.feed(buf, n)) {
            transport_->close();
            err = "malformed response: " + parser.error();
            return false;
        }
    }
    transport_->close();

    if (!parser.done()) {
        err = "response ended unexpectedly";
        return false;
    }
    out = parser.response();
    return true;
}

}  // namespace Chaos_Forger
