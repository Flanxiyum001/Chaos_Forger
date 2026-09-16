#pragma once
// ============================================================================
//  Chaos_Forger/http_socket.hpp — native HTTP/1.1 client over a POSIX UNIX socket
//
//  No libcurl, no Docker SDK, no Boost. Raw AF_UNIX SOCK_STREAM plus a
//  hand-written, feed-based HTTP/1.1 response parser.
//
//  Layers (each independently testable):
//
//    Transport            interface: connect / send_all / read_some / close
//    UnixSocketTransport  real POSIX implementation (socket/connect/send/recv/close)
//    TestTransport        feed-based fake for unit tests (see tests/test_http.cpp)
//    HttpResponseParser   incremental state machine: bytes in -> HttpResponse out
//    HttpClient           facade: request(method, path, body) -> HttpResponse
//
//  Wire policy: HTTP/1.1, one request per connection, `Connection: close`
//  so response termination is unambiguous (EOF or framing, never both).
// ============================================================================

#include <cstddef>
#include <memory>
#include <string>

namespace Chaos_Forger {

// ----------------------------------------------------------------------------
// Transport abstraction: the only seam between HTTP logic and POSIX sockets.
// ----------------------------------------------------------------------------

class Transport {
public:
    virtual ~Transport() = default;

    virtual bool connect(std::string& err) = 0;
    virtual bool send_all(const char* data, size_t len, std::string& err) = 0;

    // Returns false on error. On clean EOF, returns true with *got_eof = true
    // and len untouched. *len is bytes read into buf (0 < *len <= buf_len).
    virtual bool read_some(char* buf, size_t buf_len, size_t* len, bool* got_eof,
                           std::string& err) = 0;
    virtual void close() = 0;

    // Human-readable endpoint for diagnostics, e.g. "unix:/var/run/docker.sock".
    virtual std::string describe() const = 0;
};

// Real transport over an AF_UNIX SOCK_STREAM socket.
class UnixSocketTransport : public Transport {
public:
    explicit UnixSocketTransport(std::string socket_path, int timeout_sec = 5);
    ~UnixSocketTransport() override;

    UnixSocketTransport(const UnixSocketTransport&) = delete;
    UnixSocketTransport& operator=(const UnixSocketTransport&) = delete;

    bool connect(std::string& err) override;
    bool send_all(const char* data, size_t len, std::string& err) override;
    bool read_some(char* buf, size_t buf_len, size_t* len, bool* got_eof,
                   std::string& err) override;
    void close() override;
    std::string describe() const override;

private:
    std::string socket_path_;
    int timeout_sec_;
    int fd_ = -1;
};

// ----------------------------------------------------------------------------
// HTTP response
// ----------------------------------------------------------------------------

struct HttpResponse {
    int status = 0;
    std::string status_text;
    std::string body;

    bool ok() const { return status >= 200 && status < 300; }
    bool not_found() const { return status == 404; }
    bool conflict() const { return status == 409; }
};

// ----------------------------------------------------------------------------
// Incremental response parser — a state machine you feed bytes into.
//
//   HttpResponseParser p;
//   p.feed(buf1, n1); p.feed(buf2, n2); ...
//   if (p.done()) use(p.response());
//   else p.error() explains why parsing failed.
//
// Body framing, checked in strict precedence order:
//   1. Content-Length          — read exactly N bytes
//   2. Transfer-Encoding: chunked — decode chunks (+ chunk extensions, trailers)
//   3. EOF                     — valid only if `allow_eof_termination` is set
//                                (correct for Connection: close; this client
//                                always sends it)
// Malformed input (bad status line, non-hex chunk size, unframed body without
// EOF, oversize) is reported through error() rather than guessed at.
// ----------------------------------------------------------------------------

class HttpResponseParser {
public:
    struct Limits {
        size_t max_body_bytes = 16u * 1024u * 1024u;  // 16 MB
        size_t max_header_bytes = 64u * 1024u;        // 64 KB
        size_t max_chunk_line_bytes = 1024u;
    };

    enum class State { StatusLine, Headers, BodyContentLength, BodyChunked, BodyEof, Done, Error };

    explicit HttpResponseParser(bool allow_eof_termination = true);
    HttpResponseParser(bool allow_eof_termination, const Limits& limits);

    // Feed received bytes. Returns false if the parser entered the Error state.
    bool feed(const char* data, size_t len);

    bool done() const { return state_ == State::Done; }
    bool failed() const { return state_ == State::Error; }
    const HttpResponse& response() const { return resp_; }
    const std::string& error() const { return error_; }
    State state() const { return state_; }

private:
    bool step();                       // consume as much of buf_ as possible
    bool parse_status_line(size_t header_end);
    bool parse_headers(size_t header_end);  // sets framing_ / content_length_
    bool body_step_content_length();
    bool body_step_chunked();

    bool fail(const std::string& msg);

    std::string buf_;                  // unconsumed input
    State state_ = State::StatusLine;
    HttpResponse resp_;
    std::string error_;
    Limits limits_;

    bool allow_eof_termination_ = true;
    bool allow_eof_termination_used_ = false;
    size_t trailer_bytes_ = 0;         // running total of the trailer section

    // framing
    enum class Framing { None, ContentLength, Chunked, Eof };
    Framing framing_ = Framing::None;
    size_t content_length_ = 0;

    // chunked state
    bool chunk_started_ = false;
    size_t chunk_remaining_ = 0;
    bool chunk_trailer_done_ = false;
};

// ----------------------------------------------------------------------------
// HttpClient facade: build the request wire, send, parse the response.
// ----------------------------------------------------------------------------

class HttpClient {
public:
    explicit HttpClient(std::unique_ptr<Transport> transport);

    // Performs one full request/response cycle on a fresh connection.
    // `body` may be empty; Content-Length is always sent.
    bool request(const std::string& method, const std::string& path_with_query,
                 const std::string& body, HttpResponse& out, std::string& err);

private:
    std::unique_ptr<Transport> transport_;
};

// Request-line serializer, exposed for unit tests.
std::string build_http_request(const std::string& method, const std::string& path_with_query,
                               const std::string& body);

}  // namespace Chaos_Forger
