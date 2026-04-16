# MCP Protocol Framework

[Model Context Protocol (MCP)](https://modelcontextprotocol.io/specification/2025-03-26) is an open protocol that provides a standardized way for AI models and agents to interact with various resources, tools, and services. This framework implements the core functionality of the MCP protocol, conforming to the **2025-03-26** protocol specification.

## Core Features

- **JSON-RPC 2.0 Communication**: Request/response communication based on JSON-RPC 2.0 standard
- **Dual Transport**: Streamable HTTP (`POST/GET/DELETE /mcp`) and legacy HTTP+SSE transports
- **Tools**: Register and call tools with JSON Schema parameter validation
- **Resources**: Static resources, file resources, and URI template-based dynamic resources
- **Prompts**: Server-defined prompt templates with typed arguments
- **Logging**: Structured log delivery to clients with per-session severity filtering
- **Progress & Cancellation**: Progress notifications for long-running operations and request cancellation
- **Session Management**: Configurable session limits, inactive session timeout, and SSE keepalive
- **Pagination**: Cursor-based pagination for `tools/list`, `resources/list`, and `prompts/list`
- **SSL/TLS**: Optional OpenSSL support for HTTPS servers and clients

## How to Build

```bash
cmake -B build
cmake --build build
```

Build with tests:
```bash
cmake -B build -DMCP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --verbose
```

Build with SSL support:
```bash
cmake -B build -DMCP_SSL=ON
cmake --build build
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `MCP_BUILD_TESTS` | `OFF` | Build the test suite (uses Google Test via FetchContent) |
| `MCP_SSL` | `OFF` | Enable OpenSSL support for HTTPS |
| `MCP_MAX_SESSIONS` | `10` | Maximum concurrent sessions (0 = unlimited) |
| `MCP_SESSION_TIMEOUT` | `30` | Inactive session timeout in seconds (0 = disabled) |
| `MCP_SSE_KEEPALIVE_MS` | `30000` | SSE keepalive probe interval in milliseconds |

These can be overridden at build time (e.g. `-DMCP_MAX_SESSIONS=50`) or at runtime via the `server::configuration` struct.

### Using as a Subdirectory

When including cpp-mcp in a parent project via `add_subdirectory()`, examples and tests are automatically excluded. The CMake options above are available as cache variables:

```cmake
set(MCP_MAX_SESSIONS 20 CACHE STRING "" FORCE)
add_subdirectory(external/cpp-mcp)
target_link_libraries(my_app PRIVATE mcp)
```

## Components

### Server (`mcp_server.h`)

The server supports two transports simultaneously:

- **Streamable HTTP** (2025-03-26): `POST /mcp` for requests, `GET /mcp` for SSE server-push, `DELETE /mcp` for session termination. Sessions are identified by the `Mcp-Session-Id` header.
- **Legacy HTTP+SSE** (2024-11-05): `GET /sse` opens an SSE stream that delivers an endpoint URL, then `POST /message?session_id=...` for JSON-RPC messages.

### Client (`mcp_sse_client.h`, `mcp_stdio_client.h`)

Two client implementations:
- **SSE Client**: Connects to HTTP+SSE servers
- **Stdio Client**: Launches a subprocess and communicates over stdin/stdout

### Message Processing (`mcp_message.h`)

JSON-RPC 2.0 `request` and `response` structs with safe `from_json()` that handles absent fields per spec (e.g., notifications without `id`).

### Tools (`mcp_tool.h`)

`tool_builder` fluent API for defining tools with JSON Schema parameters:

```cpp
auto tool = mcp::tool_builder("search")
    .with_description("Search documents")
    .with_string_param("query", "Search query", true)
    .with_number_param("limit", "Max results", false)
    .build();
```

### Resources (`mcp_resource.h`)

Static resources, file resources (with auto MIME detection), and URI template-based dynamic resources:

```cpp
// Static resource
auto res = std::make_shared<mcp::text_resource>("app://status", "status", "text/plain");
res->set_text("OK");
server.register_resource("app://status", res);

// Dynamic resource template
server.register_resource_template(
    "app://items/{id}", "item", "application/json", "Item by ID",
    [](const std::string& uri, const std::map<std::string, std::string>& params,
       const std::string& session_id) -> mcp::json {
        return {{"uri", uri}, {"mimeType", "application/json"},
                {"text", get_item_json(params.at("id"))}};
    });
```

## Server Setup

```cpp
mcp::server::configuration conf;
conf.host = "0.0.0.0";
conf.port = 8080;
conf.max_sessions = 20;        // Override default
conf.session_timeout = 60;     // 60s idle timeout

mcp::server srv(conf);
srv.set_server_info("My Server", "1.0.0");
srv.set_capabilities({
    {"tools", {{"listChanged", true}}},
    {"resources", {{"subscribe", true}}},
    {"prompts", {{"listChanged", true}}},
    {"logging", json::object()}
});
```

### Registering Tools

```cpp
auto echo = mcp::tool_builder("echo")
    .with_description("Echo input back")
    .with_string_param("text", "Text to echo")
    .build();

srv.register_tool(echo, [](const mcp::json& args, const std::string& session_id) -> mcp::json {
    return mcp::json::array({{{"type", "text"}, {"text", args["text"]}}});
});
```

### Registering Prompts

```cpp
mcp::prompt summarize;
summarize.name = "summarize";
summarize.description = "Summarize a document";
summarize.arguments = {{"uri", "Document URI", true}, {"style", "Summary style", false}};

srv.register_prompt(summarize, [](const mcp::json& args, const std::string& session_id) -> mcp::json {
    return {
        {"description", "Summarize " + args.value("uri", "")},
        {"messages", mcp::json::array({
            {{"role", "user"}, {"content", {{"type", "text"},
             {"text", "Please summarize: " + args.value("uri", "")}}}}
        })}
    };
});
```

### Logging

Clients set their desired log level via `logging/setLevel`. The server filters messages per session:

```cpp
// Send a log message to a specific session
srv.send_log(session_id, "info", "my_component", "Processing started");

// Broadcast to all sessions that accept this level
srv.broadcast_log("warning", "my_component", "Rate limit approaching");
```

Log levels follow syslog severity: `debug`, `info`, `notice`, `warning`, `error`, `critical`, `alert`, `emergency`.

### Progress Notifications

For long-running tool handlers, report progress back to the client:

```cpp
srv.register_tool(long_tool, [&srv](const mcp::json& args, const std::string& session_id) -> mcp::json {
    auto token = args.value("_meta", mcp::json::object()).value("progressToken", mcp::json(nullptr));

    for (int i = 0; i < 100; i++) {
        if (srv.is_cancelled(request_id, session_id)) {
            return mcp::json::array({{{"type", "text"}, {"text", "Cancelled"}}});
        }
        if (!token.is_null()) {
            srv.send_progress(session_id, token, i, 100, "Step " + std::to_string(i));
        }
        // ... do work ...
    }
    return mcp::json::array({{{"type", "text"}, {"text", "Done"}}});
});
```

### Server-to-Client Notifications

```cpp
// Send to a specific session
auto notif = mcp::request::create_notification("resource_updated", {{"uri", "app://status"}});
srv.send_request(session_id, notif);

// Broadcast to all initialized sessions
srv.broadcast_notification(notif);

// Get connected sessions
auto sessions = srv.get_active_sessions();
```

## Client Usage

### SSE Client

```cpp
mcp::sse_client client("http://localhost:8080");
client.initialize("My Client", "1.0.0");

// Call a tool
mcp::json result = client.call_tool("echo", {{"text", "hello"}});

// List and read resources
mcp::json resources = client.list_resources();
mcp::json content = client.read_resource("app://status");

// Ping
bool alive = client.ping();
```

### Stdio Client

```cpp
mcp::stdio_client client("npx -y @modelcontextprotocol/server-filesystem /path/to/dir");
client.initialize("My Client", "1.0.0");

mcp::json resources = client.list_resources();
mcp::json result = client.call_tool("read_file", {{"path", "README.md"}});
```

## TLS / HTTPS

### Creating test certificates

```bash
openssl genrsa -out ca.key.pem 2048
openssl req -x509 -new -nodes -key ca.key.pem -sha256 -days 365 -out ca.cert.pem -subj "/CN=Test CA"
openssl genrsa -out server.key.pem 2048
openssl req -new -key server.key.pem -out server.csr.pem -subj "/CN=localhost"
openssl x509 -req -in server.csr.pem -CA ca.cert.pem -CAkey ca.key.pem -CAcreateserial -out server.cert.pem -days 365 -sha256
```

### HTTPS server

```cpp
mcp::server::configuration conf;
conf.host = "localhost";
conf.port = 8443;
conf.ssl.server_cert_path = "./server.cert.pem";
conf.ssl.server_private_key_path = "./server.key.pem";
```

### HTTPS client

```cpp
mcp::sse_client client("https://localhost:8443", "/sse", true, "./ca.cert.pem");
```

## Testing

The test suite covers JSON-RPC message format, Streamable HTTP transport, tools, resources, resource templates, prompts, logging, session management, CORS, and more:

```bash
cmake -B build -DMCP_BUILD_TESTS=ON
cmake --build build
cd build && ctest --verbose
```

Tests run on CI via GitHub Actions on every push and PR (Ubuntu and macOS).

## Adopters

- [humanus.cpp](https://github.com/WHU-MYTH-Lab/humanus.cpp): Lightweight C++ LLM agent framework

## License

MIT License. See the LICENSE file for details.
