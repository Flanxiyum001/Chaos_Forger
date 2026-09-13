// ============================================================================
//  tests/test_http.cpp — unit tests for the Forger communication layer
//
//  Dependency-free assert-based harness. Covers:
//    - build_http_request wire format
//    - HttpResponseParser: Content-Length / chunked / EOF framing, byte-by-byte
//      feeding, malformed inputs, limits
//    - Docker request-target builders and container-ID validation
//    - DockerClient::parse_container_list (canned payloads, no I/O)
//    - end-to-end parser via a scripted fake Transport
// ============================================================================

#include "forger/docker_api.hpp"
#include "forger/http_socket.hpp"
#include "forger/json.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!((a) == (b))) {                                                 \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                    \
    } while (0)

using namespace forger;

// ----------------------------------------------------------------------------
// Feed helper: push a string through the parser, optionally split.
// ----------------------------------------------------------------------------
static bool feed_all(HttpResponseParser& p, const std::string& data, size_t split) {
    if (split > 0 && split < data.size()) {
        if (!p.feed(data.data(), split)) return false;
        return p.feed(data.data() + split, data.size() - split);
    }
    return p.feed(data.data(), data.size());
}

static bool feed_eof(HttpResponseParser& p) { return p.feed(nullptr, 0); }

// ----------------------------------------------------------------------------
// build_http_request
// ----------------------------------------------------------------------------
static void test_request_builder() {
    const std::string wire = build_http_request("GET", "/v1.41/containers/json", "");
    CHECK(wire == "GET /v1.41/containers/json HTTP/1.1\r\n"
                  "Host: docker\r\n"
                  "User-Agent: Forger/1.0\r\n"
                  "Accept: application/json\r\n"
                  "Connection: close\r\n"
                  "Content-Length: 0\r\n"
                  "\r\n");

    const std::string post = build_http_request("POST", "/v1.41/x", "abc");
    CHECK(post.find("POST /v1.41/x HTTP/1.1\r\n") == 0);
    CHECK(post.find("Content-Length: 3\r\n") != std::string::npos);
    CHECK(post.substr(post.size() - 3) == "abc");
}

// ----------------------------------------------------------------------------
// Content-Length framing
// ----------------------------------------------------------------------------
static void test_content_length() {
    const std::string resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 5\r\n\r\nhello";

    HttpResponseParser p1;
    CHECK(feed_all(p1, resp, 0));
    CHECK(feed_eof(p1));
    CHECK(p1.done());
    CHECK_EQ(p1.response().status, 200);
    CHECK_EQ(p1.response().status_text, "OK");
    CHECK_EQ(p1.response().body, "hello");

    // Byte-by-byte feeding must yield the same result.
    HttpResponseParser p2;
    for (const char c : resp) CHECK(p2.feed(&c, 1));
    CHECK(feed_eof(p2));
    CHECK(p2.done());
    CHECK_EQ(p2.response().body, "hello");

    // No EOF for framed bodies: already complete.
    HttpResponseParser p3;
    CHECK(feed_all(p3, resp, 17));
    CHECK(p3.done());
}

// ----------------------------------------------------------------------------
// Chunked framing (with extensions + trailers)
// ----------------------------------------------------------------------------
static void test_chunked() {
    // "hello world" in two chunks, one with an extension, plus a trailer.
    const std::string resp =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5;ext=1\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\nX-Trailer: v\r\n\r\n";
    HttpResponseParser p;
    CHECK(feed_all(p, resp, 40));
    CHECK(feed_eof(p));
    CHECK(p.done());
    CHECK_EQ(p.response().body, "hello world");

    // No trailers.
    const std::string resp2 =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nabcd\r\n0\r\n\r\n";
    HttpResponseParser p2;
    CHECK(feed_all(p2, resp2, 0));
    CHECK(p2.done());
    CHECK_EQ(p2.response().body, "abcd");

    // Malformed: non-hex chunk size.
    const std::string bad =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n";
    HttpResponseParser p3;
    CHECK(!feed_all(p3, bad, 0));
    CHECK(p3.failed());
    CHECK(p3.error().find("chunk size") != std::string::npos);
}

// ----------------------------------------------------------------------------
// EOF-terminated body (Connection: close, no framing headers)
// ----------------------------------------------------------------------------
static void test_eof_framing() {
    const std::string resp = "HTTP/1.1 204 No Content\r\nServer: Docker/24\r\n\r\n";
    HttpResponseParser p;
    CHECK(feed_all(p, resp, 10));
    CHECK(!p.done());           // not complete until EOF
    CHECK(feed_eof(p));
    CHECK(p.done());
    CHECK_EQ(p.response().status, 204);
    CHECK_EQ(p.response().body, "");

    const std::string resp2 = "HTTP/1.1 200 OK\r\n\r\nraw-body-bytes";
    HttpResponseParser p2;
    CHECK(feed_all(p2, resp2, 0));
    CHECK(feed_eof(p2));
    CHECK_EQ(p2.response().body, "raw-body-bytes");
}

// ----------------------------------------------------------------------------
// Malformed responses
// ----------------------------------------------------------------------------
static void test_malformed() {
    struct Case { const char* resp; const char* what; };
    const Case cases[] = {
        {"NOT-HTTP\r\n\r\n", "prefix"},
        {"HTTP/2.0 200 OK\r\n\r\n", "version"},
        {"HTTP/1.1 20x OK\r\n\r\n", "code"},
        {"HTTP/1.1  200 OK\r\n\r\n", "code"},  // double space before status code
    };
    for (const Case& c : cases) {
        HttpResponseParser p;
        CHECK_EQ(feed_all(p, c.resp, 0), false);
        CHECK(p.failed());
        CHECK(p.error().find(c.what) != std::string::npos);
    }

    // Security regression: a chunk-size longer than 16 hex digits would wrap
    // strtoull (e.g. "FFFFFFFFFFFFFFFFF" -> ~18 exabytes, or "1" + 16 f's ->
    // a tiny value) and desynchronize the chunk stream. Must be rejected.
    {
        HttpResponseParser p;
        const std::string resp =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "fffffffffffffffff\r\n";  // 17 hex digits
        CHECK_EQ(feed_all(p, resp, 0), false);
        CHECK(p.failed());
        CHECK(p.error().find("chunk size too long") != std::string::npos);
    }

    // Security regression: unbounded trailer sections. Each line individually
    // fits max_chunk_line_bytes, but an endless trickle must eventually be
    // rejected (total trailer budget, not per-line only).
    {
        HttpResponseParser p;
        std::string resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n";
        for (int i = 0; i < 400; ++i) resp += "X\r\n";  // 400 tiny trailer lines
        CHECK_EQ(feed_all(p, resp, 0), false);
        CHECK(p.failed());
        CHECK(p.error().find("trailer section exceeds limit") != std::string::npos);
    }

    // Missing reason phrase is tolerated (RFC 7230: reason-phrase is optional).
    {
        HttpResponseParser p;
        CHECK(feed_all(p, "HTTP/1.1 204\r\nContent-Length: 0\r\n\r\n", 0));
        CHECK(p.done());
        CHECK_EQ(p.response().status, 204);
        CHECK(p.response().status_text.empty());
    }

    // Truncated Content-Length body.
    {
        HttpResponseParser p;
        CHECK(p.feed("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", 43));
        CHECK(!feed_eof(p));
        CHECK(p.failed());
    }

    // Unexpected extra bytes after a Content-Length body.
    {
        HttpResponseParser p;
        CHECK(!feed_all(p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nabXX", 0));
        CHECK(p.failed());
    }

    // Header block over the limit.
    {
        HttpResponseParser::Limits lim;
        lim.max_header_bytes = 32;
        HttpResponseParser p(true, lim);
        const std::string big = "HTTP/1.1 200 OK\r\nX-Big: " + std::string(200, 'a') + "\r\n\r\n";
        CHECK(!p.feed(big.data(), big.size()));
        CHECK(p.failed());
    }

    // Unframed body when EOF termination is disabled.
    {
        HttpResponseParser p(false /* allow_eof_termination */);
        CHECK(!feed_all(p, "HTTP/1.1 200 OK\r\n\r\nbody", 0));
        CHECK(p.failed());
    }
}

// ----------------------------------------------------------------------------
// Docker path builders + ID validation
// ----------------------------------------------------------------------------
static void test_docker_paths() {
    const std::string good(64, 'a');
    // Exactly 64 mixed-case hex chars.
    const std::string good_mixed = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";

    CHECK(is_valid_container_id(good));
    CHECK(is_valid_container_id(good_mixed));
    CHECK(!is_valid_container_id(good.substr(0, 63)));  // too short
    CHECK(!is_valid_container_id(good + "g"));          // bad hex
    CHECK(!is_valid_container_id(""));                  // empty
    CHECK(!is_valid_container_id("../../etc/passwd"));  // traversal attempt
    CHECK(!is_valid_container_id("abc/def"));           // path injection

    std::string out;
    CHECK(containers_json_path() == "/v1.41/containers/json?all=false");  // running only
    CHECK(ping_path() == "/_ping");

    CHECK(stop_container_path(good, 10, out));
    CHECK_EQ(out, "/v1.41/containers/" + good + "/stop?t=10");
    CHECK(!stop_container_path(good, 0, out));     // timeout < 1
    CHECK(!stop_container_path(good, 601, out));   // timeout > 600
    CHECK(!stop_container_path("short", 10, out));

    CHECK(kill_container_path(good, out));
    CHECK_EQ(out, "/v1.41/containers/" + good + "/kill?signal=SIGKILL");
    CHECK(!kill_container_path("x' OR 1=1--", out));
}

// ----------------------------------------------------------------------------
// Container-list parsing (canned payloads)
// ----------------------------------------------------------------------------
static void test_container_list_parsing() {
    const std::string id1(64, 'a');
    const std::string id2(64, 'b');

    const std::string body =
        "[{\"Id\":\"" + id1 + "\",\"Names\":[\"/web-1\"]},"
        "{\"Id\":\"" + id2 + "\",\"Names\":[\"/cache-1\",\"/alias\"]}]";

    std::vector<Container> out;
    std::string err;
    CHECK(DockerClient::parse_container_list(body, out, err));
    CHECK(err.empty());
    CHECK_EQ(out.size(), 2u);
    CHECK_EQ(out[0].id, id1);
    CHECK_EQ(out[0].names.size(), 1u);
    CHECK_EQ(out[0].names[0], "web-1");   // leading '/' stripped
    CHECK_EQ(out[1].names.size(), 2u);
    CHECK_EQ(out[1].names[1], "alias");

    // Malformed ID entries are skipped, not fatal.
    const std::string with_bad_id = "[{\"Id\":\"nope\",\"Names\":[\"/x\"]},{\"Id\":\"" + id1 + "\"}]";
    out.clear();
    CHECK(DockerClient::parse_container_list(with_bad_id, out, err));
    CHECK_EQ(out.size(), 1u);

    // Missing Id field: skipped with a warning.
    out.clear();
    CHECK(DockerClient::parse_container_list("[{\"Names\":[\"/x\"]}]", out, err));
    CHECK(out.empty());

    // Not an array.
    out.clear();
    CHECK(!DockerClient::parse_container_list("{\"Id\":\"x\"}", out, err));
    CHECK(!err.empty());

    // Broken JSON.
    out.clear();
    CHECK(!DockerClient::parse_container_list("[{", out, err));
}

// ----------------------------------------------------------------------------
// Fake Transport: end-to-end client behavior, including error reporting
// ----------------------------------------------------------------------------
class FakeTransport : public Transport {
public:
    // wire_log: optional shared sink so assertions can inspect the wire even
    // though the factory hands the client a copy of this transport.
    FakeTransport(std::string response, bool fail_connect = false, bool fail_send = false,
                  std::shared_ptr<std::string> wire_log = nullptr)
        : response_(std::move(response)), fail_connect_(fail_connect), fail_send_(fail_send),
          wire_log_(std::move(wire_log)) {}

    bool connect(std::string& err) override {
        ++connects_;
        if (fail_connect_) {
            err = "connection refused";
            return false;
        }
        return true;
    }
    bool send_all(const char* data, size_t len, std::string& err) override {
        (void)err;
        if (fail_send_) {
            err = "broken pipe";
            return false;
        }
        sent_.append(data, len);
        if (wire_log_) *wire_log_ += std::string(data, len);
        return true;
    }
    bool read_some(char* buf, size_t buf_len, size_t* len, bool* got_eof,
                   std::string& err) override {
        (void)err;
        if (pos_ >= response_.size()) {
            *got_eof = true;
            return true;
        }
        const size_t n = std::min(buf_len, response_.size() - pos_);
        std::copy(response_.data() + pos_, response_.data() + pos_ + n, buf);
        pos_ += n;
        *len = n;
        return true;
    }
    void close() override { closes_++; }
    std::string describe() const override { return "fake"; }

    int connects_ = 0;
    int closes_ = 0;
    std::string sent_;

private:
    std::string response_;
    size_t pos_ = 0;
    bool fail_connect_;
    bool fail_send_;
    std::shared_ptr<std::string> wire_log_;
};

static void test_client_end_to_end() {
    const std::string body = "[{\"Id\":\"" + std::string(64, 'f') + "\"}]";
    const std::string wire =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;

    auto wire_log = std::make_shared<std::string>();
    FakeTransport proto(wire, false, false, wire_log);
    DockerClient client([&proto]() -> std::unique_ptr<Transport> {
        return std::make_unique<FakeTransport>(proto);
    });

    std::vector<Container> containers;
    std::string err;
    CHECK(client.list_containers(containers, err));
    CHECK_EQ(containers.size(), 1u);

    // The request line must carry the pinned API version.
    CHECK(wire_log->find("GET /v1.41/containers/json?all=false HTTP/1.1\r\n") == 0);
    CHECK(wire_log->find("Connection: close\r\n") != std::string::npos);
}

static void test_client_error_paths() {
    // Connect failure -> clear error, no crash.
    {
        DockerClient client([]() -> std::unique_ptr<Transport> {
            return std::make_unique<FakeTransport>("", true /* fail_connect */);
        });
        std::vector<Container> out;
        std::string err;
        CHECK(!client.list_containers(out, err));
        CHECK(err.find("connect to") != std::string::npos);
        CHECK(err.find("connection refused") != std::string::npos);
    }

    // Send failure.
    {
        DockerClient client([]() -> std::unique_ptr<Transport> {
            return std::make_unique<FakeTransport>("", false, true /* fail_send */);
        });
        std::string err;
        CHECK(!client.ping(err));
        CHECK(err.find("send failed") != std::string::npos);
    }

    // Connection dies mid-response -> malformed, not a hang.
    {
        DockerClient client([]() -> std::unique_ptr<Transport> {
            return std::make_unique<FakeTransport>(
                "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly-a-bit");
        });
        std::vector<Container> out;
        std::string err;
        CHECK(!client.list_containers(out, err));
        CHECK(err.find("malformed") != std::string::npos ||
              err.find("closed") != std::string::npos);
    }

    // HTTP 500 surfaces with status + body excerpt.
    {
        const std::string err_body = "{\"message\":\"boom\"}";
        DockerClient client([&err_body]() -> std::unique_ptr<Transport> {
            return std::make_unique<FakeTransport>(
                "HTTP/1.1 500 Internal Server Error\r\nContent-Length: " +
                std::to_string(err_body.size()) + "\r\n\r\n" + err_body);
        });
        std::string err;
        CHECK(!client.ping(err));
        CHECK(err.find("500") != std::string::npos);
    }
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------
int main() {
    test_request_builder();
    test_content_length();
    test_chunked();
    test_eof_framing();
    test_malformed();
    test_docker_paths();
    test_container_list_parsing();
    test_client_end_to_end();
    test_client_error_paths();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
