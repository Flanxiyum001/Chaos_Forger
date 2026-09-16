#pragma once
// ============================================================================
//  Chaos_Forger/docker_api.hpp — Docker Engine API client (version-pinned v1.41)
//
//  Pure path/ID validation helpers are free functions so unit tests can pin
//  down request construction without any I/O.
// ============================================================================

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Chaos_Forger/http_socket.hpp"

namespace Chaos_Forger {

inline constexpr const char* kDockerApiVersion = "v1.41";

struct Container {
    std::string id;                  // full 64-char hex id
    std::vector<std::string> names;  // normalized: no leading '/'
    std::string image;               // e.g. "nginx:alpine"
    std::string state;               // e.g. "running"
    std::string status;              // e.g. "Up 2 minutes"
};

// Docker returns container names with a leading '/' ("/web-app"); normalize to
// the plain name ("web-app") so matching behaves predictably.
std::string normalize_container_name(std::string raw);

// --- Request-target builders (pure, unit-testable) --------------------------
//
// Container IDs come from the Engine's own JSON, but they are still untrusted
// input: they are embedded into request paths, and the socket is
// root-equivalent. A malformed ID must never reach the wire.

// Valid Engine container IDs are 64 lowercase hex chars (Docker may also
// accept prefixed forms; Chaos_Forger only forwards IDs it received verbatim from
// /containers/json, so the strict form is enforced here).
bool is_valid_container_id(const std::string& id);

// "GET /v1.41/containers/json?all=false" — running containers only (explicit).
std::string containers_json_path();

// "POST /v1.41/containers/<id>/stop?t=<n>"  (n in [1, 600])
// Returns false if the id is invalid or the timeout is out of range.
bool stop_container_path(const std::string& id, int timeout_seconds, std::string& out);

// "POST /v1.41/containers/<id>/kill?signal=SIGKILL"
bool kill_container_path(const std::string& id, std::string& out);

// "GET /_ping" (unversioned endpoint, supported across Engine versions)
std::string ping_path();

// --- Client interface ---------------------------------------------------------
//
// The chaos engine depends on this abstract interface, not on DockerClient:
// unit tests inject a fake (tests/test_engine.cpp) and need no Docker daemon,
// no socket, and no network. The interface is exactly the surface the engine
// uses — nothing more, so fakes stay trivial to write.

class IDockerClient {
public:
    virtual ~IDockerClient() = default;

    virtual bool list_containers(std::vector<Container>& out, std::string& err) = 0;
    virtual bool stop_container(const std::string& id, int timeout_seconds, std::string& err,
                                int* http_status = nullptr) = 0;
    virtual bool kill_container(const std::string& id, std::string& err,
                                int* http_status = nullptr) = 0;
    virtual bool ping(std::string& err) = 0;
};

// The Engine API is stateless and Chaos_Forger uses `Connection: close`, so every
// request gets a fresh connection: the factory is invoked once per request.
using TransportFactory = std::function<std::unique_ptr<Transport>()>;

class DockerClient final : public IDockerClient {
public:
    explicit DockerClient(TransportFactory make_transport);

    // Convenience factory for the real socket.
    static TransportFactory unix_socket_factory(std::string socket_path, int timeout_sec = 5);

    // GET /v1.41/containers/json — running containers only.
    bool list_containers(std::vector<Container>& out, std::string& err) override;

    // POST /v1.41/containers/<id>/stop?t=<seconds>
    // On any completed HTTP exchange, *http_status (when non-null) receives the
    // Engine's status code; transport-level failures leave it 0 (unknown).
    bool stop_container(const std::string& id, int timeout_seconds, std::string& err,
                        int* http_status = nullptr) override;

    // POST /v1.41/containers/<id>/kill?signal=SIGKILL
    bool kill_container(const std::string& id, std::string& err, int* http_status = nullptr) override;

    // GET /_ping — reachability probe.
    bool ping(std::string& err) override;

    // Reuse an already-parsed response (unit tests inject canned payloads).
    static bool parse_container_list(const std::string& body, std::vector<Container>& out,
                                     std::string& err);

private:
    bool post_container_action(const std::string& path, const std::string& what,
                               HttpResponse& out, std::string& err, int* http_status);
    TransportFactory make_transport_;
};

}  // namespace Chaos_Forger
