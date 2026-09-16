// ============================================================================
//  src/docker_api.cpp — Docker Engine API client (version-pinned v1.41)
// ============================================================================

#include "Chaos_Forger/docker_api.hpp"

#include <cctype>
#include <string>
#include <vector>

#include "Chaos_Forger/json.hpp"
#include "Chaos_Forger/log.hpp"

namespace Chaos_Forger {

bool is_valid_container_id(const std::string& id) {
    if (id.size() != 64) return false;
    for (const char c : id) {
        const bool hex_ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F');
        if (!hex_ok) return false;
    }
    return true;
}

std::string containers_json_path() {
    // ?all=false is the default but stated explicitly: Chaos_Forger targets running
    // containers only and must keep doing so even if defaults ever change.
    return std::string("/") + kDockerApiVersion + "/containers/json?all=false";
}

std::string normalize_container_name(std::string raw) {
    while (!raw.empty() && raw.front() == '/') raw.erase(0, 1);
    return raw;
}

bool stop_container_path(const std::string& id, int timeout_seconds, std::string& out) {
    if (!is_valid_container_id(id)) return false;
    if (timeout_seconds < 1 || timeout_seconds > 600) return false;
    out = std::string("/") + kDockerApiVersion + "/containers/" + id + "/stop?t=" +
          std::to_string(timeout_seconds);
    return true;
}

bool kill_container_path(const std::string& id, std::string& out) {
    if (!is_valid_container_id(id)) return false;
    out = std::string("/") + kDockerApiVersion + "/containers/" + id + "/kill?signal=SIGKILL";
    return true;
}

std::string ping_path() { return "/_ping"; }

// ============================================================================
// DockerClient
// ============================================================================

DockerClient::DockerClient(TransportFactory make_transport)
    : make_transport_(std::move(make_transport)) {}

TransportFactory DockerClient::unix_socket_factory(std::string socket_path, int timeout_sec) {
    return [path = std::move(socket_path), timeout_sec]() -> std::unique_ptr<Transport> {
        return std::make_unique<UnixSocketTransport>(path, timeout_sec);
    };
}

bool DockerClient::post_container_action(const std::string& path, const std::string& what,
                                         HttpResponse& out, std::string& err, int* http_status) {
    if (http_status != nullptr) *http_status = 0;  // unknown until a response arrives
    HttpClient client(make_transport_());

    if (!client.request("POST", path, "", out, err)) {
        err = what + ": " + err;
        return false;
    }
    if (http_status != nullptr) *http_status = out.status;
    if (!out.ok()) {
        err = what + ": HTTP " + std::to_string(out.status) +
              (out.body.empty() ? "" : " -- " + out.body.substr(0, 200));
        return false;
    }
    return true;
}

bool DockerClient::list_containers(std::vector<Container>& out, std::string& err) {
    HttpClient client(make_transport_());

    HttpResponse resp;
    if (!client.request("GET", containers_json_path(), "", resp, err)) {
        err = "listing containers: " + err;
        return false;
    }
    if (!resp.ok()) {
        err = "listing containers: HTTP " + std::to_string(resp.status) +
              (resp.body.empty() ? "" : " -- " + resp.body.substr(0, 300));
        return false;
    }
    return parse_container_list(resp.body, out, err);
}

bool DockerClient::parse_container_list(const std::string& body, std::vector<Container>& out,
                                        std::string& err) {
    Json root;
    if (!JsonParser::parse(body, root, err)) {
        err = "cannot parse container list JSON: " + err;
        return false;
    }
    if (!root.is_array()) {
        err = "container list: expected a JSON array";
        return false;
    }

    for (const Json& entry : root.as_array()) {
        const Json* id = entry.find("Id");
        if (!id || !id->is_string()) {
            LOG_WARN("container list: entry without string 'Id' field, skipping");
            continue;
        }
        Container c;
        c.id = id->as_string();
        if (!is_valid_container_id(c.id)) {
            // Never forward a malformed ID into a request path; log loudly.
            LOG_WARN("container list: entry with malformed Id (len=" +
                     std::to_string(c.id.size()) + "), skipping");
            continue;
        }
        if (const Json* names = entry.find("Names")) {
            if (names->is_array()) {
                for (const Json& n : names->as_array()) {
                    if (!n.is_string()) continue;
                    std::string name = normalize_container_name(n.as_string());
                    if (!name.empty()) c.names.push_back(std::move(name));
                }
            }
        }
        // Descriptive fields (discovery/logging only — never used for matching).
        if (const Json* v = entry.find("Image")) {
            if (v->is_string()) c.image = v->as_string();
        }
        if (const Json* v = entry.find("State")) {
            if (v->is_string()) c.state = v->as_string();
        }
        if (const Json* v = entry.find("Status")) {
            if (v->is_string()) c.status = v->as_string();
        }
        out.push_back(std::move(c));
    }
    return true;
}

bool DockerClient::stop_container(const std::string& id, int timeout_seconds, std::string& err,
                                  int* http_status) {
    std::string path;
    if (!stop_container_path(id, timeout_seconds, path)) {
        err = "stop: invalid container id or timeout";
        return false;
    }
    HttpResponse resp;
    return post_container_action(path, "stop", resp, err, http_status);
}

bool DockerClient::kill_container(const std::string& id, std::string& err, int* http_status) {
    std::string path;
    if (!kill_container_path(id, path)) {
        err = "kill: invalid container id";
        return false;
    }
    HttpResponse resp;
    return post_container_action(path, "kill", resp, err, http_status);
}

bool DockerClient::ping(std::string& err) {
    HttpClient client(make_transport_());

    HttpResponse resp;
    if (!client.request("GET", ping_path(), "", resp, err)) return false;
    if (!resp.ok()) {
        err = "ping: HTTP " + std::to_string(resp.status);
        return false;
    }
    return true;
}

}  // namespace Chaos_Forger
