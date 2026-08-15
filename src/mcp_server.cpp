/**
 * @file mcp_server.cpp
 * @brief Implementation of the MCP server
 * 
 * This file implements the server-side functionality for the Model Context Protocol.
 * Follows the 2024-11-05 basic protocol specification.
 */

#include "mcp_server.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <random>
#include <regex>
#include <sys/stat.h>

namespace {
bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool starts_with_case_insensitive(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool is_valid_jsonrpc_id(const mcp::json& id) {
    return id.is_null() || id.is_string() || id.is_number_integer() || id.is_number_unsigned();
}

mcp::json request_id_or_null(const mcp::json& message) {
    if (message.is_object() && message.contains("id") && is_valid_jsonrpc_id(message["id"])) {
        return message["id"];
    }
    return nullptr;
}

void set_jsonrpc_error(httplib::Response& res,
                       int status,
                       const mcp::json& id,
                       mcp::error_code code,
                       const std::string& message) {
    res.status = status;
    res.set_content(mcp::response::create_error(id, code, message).to_json().dump(),
                    "application/json");
}

std::string quoted_auth_parameter(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '\r' || c == '\n') {
            continue;
        }
        if (c == '\\' || c == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

std::string mcp_exception_message(const mcp::mcp_exception& exception,
                                  bool expose_error_details,
                                  const std::string& fallback) {
    if (exception.code() == mcp::error_code::internal_error &&
        !expose_error_details) {
        return fallback;
    }
    return exception.what();
}

bool validate_schema_type(const mcp::json& schema,
                          const mcp::json& value,
                          const std::string& path,
                          std::string& error);

size_t utf8_code_point_count(const std::string& value) {
    size_t count = 0;
    for (unsigned char c : value) {
        if ((c & 0xc0) != 0x80) {
            ++count;
        }
    }
    return count;
}

bool schema_type_matches(const std::string& type, const mcp::json& value) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "number") return value.is_number();
    if (type == "integer") {
        return value.is_number_integer() || value.is_number_unsigned();
    }
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return false;
}

bool validate_schema_combinators(const mcp::json& schema,
                                 const mcp::json& value,
                                 const std::string& path,
                                 std::string& error) {
    if (schema.contains("allOf")) {
        if (!schema["allOf"].is_array()) {
            error = path + " has invalid allOf schema";
            return false;
        }
        for (const auto& subschema : schema["allOf"]) {
            if (!validate_schema_type(subschema, value, path, error)) {
                return false;
            }
        }
    }

    if (schema.contains("anyOf")) {
        if (!schema["anyOf"].is_array() || schema["anyOf"].empty()) {
            error = path + " has invalid anyOf schema";
            return false;
        }
        bool matched = false;
        for (const auto& subschema : schema["anyOf"]) {
            std::string ignored;
            if (validate_schema_type(subschema, value, path, ignored)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            error = path + " must match at least one anyOf schema";
            return false;
        }
    }

    if (schema.contains("oneOf")) {
        if (!schema["oneOf"].is_array() || schema["oneOf"].empty()) {
            error = path + " has invalid oneOf schema";
            return false;
        }
        size_t matches = 0;
        for (const auto& subschema : schema["oneOf"]) {
            std::string ignored;
            if (validate_schema_type(subschema, value, path, ignored)) {
                ++matches;
            }
        }
        if (matches != 1) {
            error = path + " must match exactly one oneOf schema";
            return false;
        }
    }

    if (schema.contains("not")) {
        std::string ignored;
        if (validate_schema_type(schema["not"], value, path, ignored)) {
            error = path + " matches a forbidden schema";
            return false;
        }
    }

    return true;
}

bool validate_object_schema(const mcp::json& schema,
                            const mcp::json& value,
                            const std::string& path,
                            std::string& error) {
    if (!value.is_object()) {
        error = path + " must be an object";
        return false;
    }

    if (schema.contains("minProperties") && schema["minProperties"].is_number_unsigned() &&
        value.size() < schema["minProperties"].get<size_t>()) {
        error = path + " has too few properties";
        return false;
    }
    if (schema.contains("maxProperties") && schema["maxProperties"].is_number_unsigned() &&
        value.size() > schema["maxProperties"].get<size_t>()) {
        error = path + " has too many properties";
        return false;
    }

    if (schema.contains("required") && !schema["required"].is_array()) {
        error = path + " has invalid required schema";
        return false;
    }
    if (schema.contains("required")) {
        for (const auto& required : schema["required"]) {
            if (!required.is_string()) {
                error = path + " has non-string required property";
                return false;
            }
            const auto key = required.get<std::string>();
            if (!value.contains(key)) {
                error = "Missing required parameter '" + key + "'";
                return false;
            }
        }
    }

    if (schema.contains("properties") && !schema["properties"].is_object()) {
        error = path + " has invalid properties schema";
        return false;
    }
    if (schema.contains("properties")) {
        const auto& properties = schema["properties"];
        for (const auto& [key, property_schema] : properties.items()) {
            if (!value.contains(key)) {
                continue;
            }
            if (!validate_schema_type(property_schema, value[key], path + "." + key, error)) {
                return false;
            }
        }
    }

    if (schema.contains("additionalProperties")) {
        const auto& additional = schema["additionalProperties"];
        if (!additional.is_boolean() && !additional.is_object()) {
            error = path + " has invalid additionalProperties schema";
            return false;
        }
        for (const auto& [key, item] : value.items()) {
            const bool declared = schema.contains("properties") &&
                                  schema["properties"].contains(key);
            if (declared) {
                continue;
            }
            if (additional.is_boolean() && !additional.get<bool>()) {
                error = "Unexpected parameter '" + key + "'";
                return false;
            }
            if (additional.is_object() &&
                !validate_schema_type(additional, item, path + "." + key, error)) {
                return false;
            }
        }
    }

    return true;
}

bool validate_schema_type(const mcp::json& schema,
                          const mcp::json& value,
                          const std::string& path,
                          std::string& error) {
    if (schema.is_boolean()) {
        if (schema.get<bool>()) {
            return true;
        }
        error = path + " is not allowed";
        return false;
    }
    if (!schema.is_object()) {
        error = path + " has an invalid JSON Schema";
        return false;
    }

    static const std::array<const char*, 13> unsupported_keywords = {
        "$ref", "$dynamicRef", "contains", "dependentSchemas", "else", "if",
        "maxContains", "minContains", "patternProperties", "prefixItems",
        "propertyNames", "then", "unevaluatedProperties"
    };
    for (const char* keyword : unsupported_keywords) {
        if (schema.contains(keyword)) {
            error = path + " uses unsupported JSON Schema keyword '" + keyword + "'";
            return false;
        }
    }

    if (schema.contains("const") && value != schema["const"]) {
        error = path + " must equal the schema's const value";
        return false;
    }
    if (schema.contains("enum")) {
        if (!schema["enum"].is_array() || schema["enum"].empty()) {
            error = path + " has invalid enum schema";
            return false;
        }
        bool matched = false;
        for (const auto& candidate : schema["enum"]) {
            if (candidate == value) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            error = path + " must be one of the allowed values";
            return false;
        }
    }

    if (!validate_schema_combinators(schema, value, path, error)) {
        return false;
    }

    if (schema.contains("type")) {
        bool matched = false;
        if (schema["type"].is_string()) {
            const auto expected_type = schema["type"].get<std::string>();
            matched = schema_type_matches(expected_type, value);
            if (!matched) {
                error = path + " must be of type '" + expected_type + "'";
                return false;
            }
        } else if (schema["type"].is_array()) {
            for (const auto& type : schema["type"]) {
                if (!type.is_string()) {
                    error = path + " has invalid type schema";
                    return false;
                }
                if (schema_type_matches(type.get<std::string>(), value)) {
                    matched = true;
                }
            }
        } else {
            error = path + " has invalid type schema";
            return false;
        }
        if (!matched) {
            error = path + " has the wrong type";
            return false;
        }
    }

    if (value.is_object()) {
        return validate_object_schema(schema, value, path, error);
    }

    if (value.is_array()) {
        if (schema.contains("minItems") && schema["minItems"].is_number_unsigned() &&
            value.size() < schema["minItems"].get<size_t>()) {
            error = path + " has too few items";
            return false;
        }
        if (schema.contains("maxItems") && schema["maxItems"].is_number_unsigned() &&
            value.size() > schema["maxItems"].get<size_t>()) {
            error = path + " has too many items";
            return false;
        }
        if (schema.contains("uniqueItems") && !schema["uniqueItems"].is_boolean()) {
            error = path + " has invalid uniqueItems schema";
            return false;
        }
        if (schema.value("uniqueItems", false)) {
            for (size_t i = 0; i < value.size(); ++i) {
                for (size_t j = i + 1; j < value.size(); ++j) {
                    if (value[i] == value[j]) {
                        error = path + " must contain unique items";
                        return false;
                    }
                }
            }
        }
        if (schema.contains("items")) {
            for (size_t i = 0; i < value.size(); ++i) {
                if (!validate_schema_type(schema["items"], value[i],
                                          path + "[" + std::to_string(i) + "]", error)) {
                    return false;
                }
            }
        }
        return true;
    }

    if (value.is_string()) {
        const auto string_value = value.get<std::string>();
        const size_t length = utf8_code_point_count(string_value);
        if (schema.contains("minLength") && schema["minLength"].is_number_unsigned() &&
            length < schema["minLength"].get<size_t>()) {
            error = path + " is too short";
            return false;
        }
        if (schema.contains("maxLength") && schema["maxLength"].is_number_unsigned() &&
            length > schema["maxLength"].get<size_t>()) {
            error = path + " is too long";
            return false;
        }
        if (schema.contains("pattern")) {
            if (!schema["pattern"].is_string()) {
                error = path + " has invalid pattern schema";
                return false;
            }
            try {
                const std::regex pattern(schema["pattern"].get<std::string>());
                if (!std::regex_search(string_value, pattern)) {
                    error = path + " does not match the required pattern";
                    return false;
                }
            } catch (const std::regex_error&) {
                error = path + " has invalid pattern schema";
                return false;
            }
        }
        return true;
    }

    if (value.is_number()) {
        const double number = value.get<double>();
        if (schema.contains("minimum") && schema["minimum"].is_number() &&
            number < schema["minimum"].get<double>()) {
            error = path + " is below the minimum";
            return false;
        }
        if (schema.contains("maximum") && schema["maximum"].is_number() &&
            number > schema["maximum"].get<double>()) {
            error = path + " is above the maximum";
            return false;
        }
        if (schema.contains("exclusiveMinimum") && schema["exclusiveMinimum"].is_number() &&
            number <= schema["exclusiveMinimum"].get<double>()) {
            error = path + " must be greater than the exclusive minimum";
            return false;
        }
        if (schema.contains("exclusiveMaximum") && schema["exclusiveMaximum"].is_number() &&
            number >= schema["exclusiveMaximum"].get<double>()) {
            error = path + " must be less than the exclusive maximum";
            return false;
        }
        if (schema.contains("multipleOf") && schema["multipleOf"].is_number()) {
            const double divisor = schema["multipleOf"].get<double>();
            if (divisor <= 0.0 ||
                std::abs(std::remainder(number, divisor)) > 1e-9 * std::max(1.0, std::abs(number))) {
                error = path + " must be a multiple of the configured value";
                return false;
            }
        }
    }

    return true;
}

int64_t unix_now() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool validate_tool_arguments(const mcp::json& schema,
                             const mcp::json& args,
                             std::string& error) {
    return validate_schema_type(schema, args, "arguments", error);
}
} // anonymous namespace

namespace mcp {


server::server(const server::configuration& conf)
    : host_(conf.host)
    , port_(conf.port)
    , name_(conf.name)
    , version_(conf.version)
    , sse_endpoint_(conf.sse_endpoint)
    , msg_endpoint_(conf.msg_endpoint)
    , mcp_endpoint_(conf.mcp_endpoint)
    , thread_pool_(conf.threadpool_size, conf.max_queued_tasks)
    , max_sessions_(conf.max_sessions)
    , session_timeout_(conf.session_timeout)
    , allowed_origins_(conf.allowed_origins)
    , enable_legacy_sse_transport_(conf.enable_legacy_sse_transport)
    , expose_error_details_(conf.expose_error_details)
    , auth_resource_metadata_url_(conf.auth_resource_metadata_url)
    , max_request_body_size_(conf.max_request_body_size)
    , max_queued_http_requests_(conf.max_queued_http_requests)
    , http_thread_pool_size_(conf.http_thread_pool_size > 0 ? conf.http_thread_pool_size : 64)
{
    #ifdef MCP_SSL
    if (conf.ssl.server_cert_path && conf.ssl.server_private_key_path) {
        if (!file_exists(*conf.ssl.server_cert_path)) {
            LOG_ERROR("SSL certificate file '", *conf.ssl.server_cert_path, "' not found");
        }

        if (!file_exists(*conf.ssl.server_private_key_path)) {
            LOG_ERROR("SSL key file '", *conf.ssl.server_private_key_path, "' not found");
        }

        http_server_ = std::make_unique<httplib::SSLServer>(conf.ssl.server_cert_path->c_str(),
            conf.ssl.server_private_key_path->c_str());
    } else {
        http_server_ = std::make_unique<httplib::Server>();
    }
    #else
     http_server_ = std::make_unique<httplib::Server>();
    #endif

    // Override httplib's default task queue (max(8, hardware_concurrency()-1)).
    // Each active SSE chunked content provider parks one httplib worker for
    // the lifetime of the stream, which on small embedded hosts (4-core
    // NanoPi) means a handful of concurrent clients can starve the server of
    // capacity to handle anything else — including the POST traffic those
    // same SSE clients depend on. Setting our own pool size decouples the
    // ceiling from hardware_concurrency.
    unsigned int pool_size = http_thread_pool_size_;
    size_t max_queued_http_requests = max_queued_http_requests_;
    http_server_->new_task_queue = [pool_size, max_queued_http_requests]() {
        return new httplib::ThreadPool(pool_size, max_queued_http_requests);
    };

    if (max_request_body_size_ > 0) {
        http_server_->set_payload_max_length(max_request_body_size_);
    }

    http_server_->set_exception_handler([this](const httplib::Request& req,
                                               httplib::Response& res,
                                               std::exception_ptr ep) {
        try {
            if (ep) {
                std::rethrow_exception(ep);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Unhandled HTTP handler exception: ", e.what());
        } catch (...) {
            LOG_ERROR("Unhandled unknown HTTP handler exception");
        }

        if (origin_is_allowed(req)) {
            set_cors_headers(req, res, "GET, POST, DELETE, OPTIONS");
        }
        set_jsonrpc_error(res, 500, nullptr, error_code::internal_error, "Internal error");
    });
}

server::~server() {
    stop();
}


bool server::start(bool blocking) {
    if (running_) {
        return true;  // Already running
    }
    
    LOG_INFO("Starting MCP server on ", host_, ":", port_);
    
    // Setup CORS handling
    http_server_->Options(".*", [this](const httplib::Request& req, httplib::Response& res) {
        if (!origin_is_allowed(req)) {
            res.status = 403;
            res.set_content("{\"error\":\"Forbidden origin\"}", "application/json");
            return;
        }
        set_cors_headers(req, res, "GET, POST, DELETE, OPTIONS");
        res.status = 204; // No Content
    });

    if (enable_legacy_sse_transport_) {
        // Setup JSON-RPC endpoint (legacy HTTP+SSE transport)
        http_server_->Post(msg_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
            this->handle_jsonrpc(req, res);
            LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" ", res.status);
        });

        // Setup SSE endpoint (legacy 2024-11-05 transport)
        http_server_->Get(sse_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
            this->handle_sse(req, res);
            LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" ", res.status);
        });
    }

    // Streamable HTTP transport (2025-03-26)
    http_server_->Post(mcp_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_mcp_post(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" ", res.status);
    });

    http_server_->Get(mcp_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_mcp_get(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" ", res.status);
    });

    http_server_->Delete(mcp_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_mcp_delete(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"DELETE ", req.path, " HTTP/1.1\" ", res.status);
    });
    
    start_maintenance_thread();
    
    // Start server
    if (blocking) {
        running_ = true;
        LOG_INFO("Starting server in blocking mode");
        if (!http_server_->listen(host_.c_str(), port_)) {
            running_ = false;
            stop_maintenance_thread();
            LOG_ERROR("Failed to start server on ", host_, ":", port_);
            return false;
        }
        return true;
    } else {
        // Start server in a separate thread
        server_thread_ = std::make_unique<std::thread>([this]() {
            LOG_INFO("Starting server in separate thread");
            if (!http_server_->listen(host_.c_str(), port_)) {
                LOG_ERROR("Failed to start server on ", host_, ":", port_);
                running_ = false;
                stop_maintenance_thread();
                return;
            }
        });
        running_ = true;
        return true;
    }
}

void server::stop() {
    if (!running_) {
        return;
    }
    
    LOG_INFO("Stopping MCP server on ", host_, ":", port_);
    running_ = false;

    stop_maintenance_thread();
    
    // Copy all dispatchers and threads to avoid holding the lock for too long
    std::vector<std::shared_ptr<event_dispatcher>> dispatchers_to_close;
    std::vector<std::unique_ptr<std::thread>> threads_to_join;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Copy all dispatchers
        dispatchers_to_close.reserve(session_dispatchers_.size());
        for (const auto& [_, dispatcher] : session_dispatchers_) {
            dispatchers_to_close.push_back(dispatcher);
        }

        // Copy all threads
        threads_to_join.reserve(sse_threads_.size());
        for (auto& [_, thread] : sse_threads_) {
            if (thread && thread->joinable()) {
                threads_to_join.push_back(std::move(thread));
            }
        }

        // Clear the maps
        session_dispatchers_.clear();
        sse_threads_.clear();
        session_initialized_.clear();
        session_protocol_versions_.clear();
        session_log_levels_.clear();
        cancelled_requests_.clear();
    }

    // Close all copied dispatchers so any threads waiting in wait_event()
    // wake up immediately instead of blocking on the keepalive timeout.
    for (auto& dispatcher : dispatchers_to_close) {
        if (dispatcher) {
            dispatcher->close();
        }
    }

    // Give threads some time to handle close events
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Join all SSE threads unconditionally. We already closed their dispatchers
    // above, which wakes them from wait_event / wait_for_close immediately,
    // so join should complete within milliseconds. Detach is never safe here
    // because the threads access server state (e.g. via close_session) and
    // would cause use-after-free if they outlive the server.
    for (auto& thread : threads_to_join) {
        if (thread && thread->joinable()) {
            try {
                thread->join();
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to join SSE thread: ", e.what());
            }
        }
    }

    if (server_thread_ && server_thread_->joinable()) {
        http_server_->stop();
        try {
            server_thread_->join();
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to join server thread: ", e.what());
        }
    } else {
        http_server_->stop();
    }
    
    LOG_INFO("MCP server stopped");
}

bool server::is_running() const {
    return running_;
}

void server::set_server_info(const std::string& name, const std::string& version) {
    std::lock_guard<std::mutex> lock(mutex_);
    name_ = name;
    version_ = version;
}

void server::set_capabilities(const json& capabilities) {
    std::lock_guard<std::mutex> lock(mutex_);
    capabilities_ = capabilities;
}

void server::set_instructions(const std::string& instructions) {
    std::lock_guard<std::mutex> lock(mutex_);
    instructions_ = instructions;
}

void server::register_method(const std::string& method, method_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    method_handlers_[method] = handler;
}

void server::register_notification(const std::string& method, notification_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    notification_handlers_[method] = handler;
}

// Simple URI template matching: extracts {param} segments from a template.
// e.g. "myapp://items/{id}" matches "myapp://items/abc" with params["id"]="abc"
static bool match_uri_template(const std::string& tmpl,
                               const std::string& uri,
                               std::map<std::string, std::string>& params)
{
    params.clear();
    size_t ti = 0, ui = 0;
    while (ti < tmpl.size() && ui < uri.size()) {
        if (tmpl[ti] == '{') {
            size_t end = tmpl.find('}', ti);
            if (end == std::string::npos) return false;
            std::string key = tmpl.substr(ti + 1, end - ti - 1);
            ti = end + 1;
            // Consume URI chars until we hit the next literal from the template (or end)
            size_t val_end;
            if (ti < tmpl.size()) {
                val_end = uri.find(tmpl[ti], ui);
                if (val_end == std::string::npos) return false;
            } else {
                val_end = uri.size();
            }
            params[key] = uri.substr(ui, val_end - ui);
            ui = val_end;
        } else {
            if (tmpl[ti] != uri[ui]) return false;
            ++ti;
            ++ui;
        }
    }
    return ti == tmpl.size() && ui == uri.size();
}

void server::register_resource(const std::string& path, std::shared_ptr<resource> resource) {
    std::lock_guard<std::mutex> lock(mutex_);
    resources_[path] = resource;

    // Register methods for resource access
    if (method_handlers_.find("resources/read") == method_handlers_.end()) {
        method_handlers_["resources/read"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri") || !params["uri"].is_string()) {
                throw mcp_exception(error_code::invalid_params, "Missing or invalid 'uri' parameter");
            }

            std::string uri = params["uri"];

            // Try static resources first
            auto it = resources_.find(uri);
            if (it != resources_.end()) {
                json contents = json::array();
                contents.push_back(it->second->read());
                return json{{"contents", contents}};
            }

            // Try resource templates
            for (const auto& tmpl : resource_templates_) {
                std::map<std::string, std::string> uri_params;
                if (match_uri_template(tmpl.uri_template, uri, uri_params)) {
                    json result = tmpl.handler(uri, uri_params, session_id);
                    json contents = json::array();
                    contents.push_back(result);
                    return json{{"contents", contents}};
                }
            }

            throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
        };
    }
    
    if (method_handlers_.find("resources/list") == method_handlers_.end()) {
        method_handlers_["resources/list"] = [this](const json& params, const std::string& session_id) -> json {
            // Cursor-based pagination: cursor is the index to start from
            size_t start = 0;
            size_t page_size = 1000;
            if (params.contains("cursor") && params["cursor"].is_string()) {
                try { start = std::stoul(params["cursor"].get<std::string>()); } catch (...) {}
            }

            json resources = json::array();
            size_t idx = 0;
            for (const auto& [uri, res] : resources_) {
                if (idx >= start && resources.size() < page_size) {
                    resources.push_back(res->get_metadata());
                }
                idx++;
            }

            json result = {{"resources", resources}};
            if (start + page_size < resources_.size()) {
                result["nextCursor"] = std::to_string(start + page_size);
            }
            return result;
        };
    }
    
    if (method_handlers_.find("resources/subscribe") == method_handlers_.end()) {
        method_handlers_["resources/subscribe"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri") || !params["uri"].is_string()) {
                throw mcp_exception(error_code::invalid_params, "Missing or invalid 'uri' parameter");
            }
            
            std::string uri = params["uri"];
            auto it = resources_.find(uri);
            if (it == resources_.end()) {
                throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
            }
            
            return json::object();
        };
    }

    if (method_handlers_.find("resources/unsubscribe") == method_handlers_.end()) {
        method_handlers_["resources/unsubscribe"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri") || !params["uri"].is_string()) {
                throw mcp_exception(error_code::invalid_params, "Missing or invalid 'uri' parameter");
            }
            return json::object();
        };
    }

    if (method_handlers_.find("resources/templates/list") == method_handlers_.end()) {
        method_handlers_["resources/templates/list"] = [this](const json& params, const std::string& session_id) -> json {
            size_t start = 0;
            size_t page_size = 1000;
            if (params.contains("cursor") && params["cursor"].is_string()) {
                try { start = std::stoul(params["cursor"].get<std::string>()); } catch (...) {}
            }

            json templates_json = json::array();
            for (size_t i = start; i < resource_templates_.size() && templates_json.size() < page_size; i++) {
                const auto& tmpl = resource_templates_[i];
                templates_json.push_back({
                    {"uriTemplate", tmpl.uri_template},
                    {"name", tmpl.name},
                    {"description", tmpl.description},
                    {"mimeType", tmpl.mime_type}
                });
            }

            json result = {{"resourceTemplates", templates_json}};
            if (start + page_size < resource_templates_.size()) {
                result["nextCursor"] = std::to_string(start + page_size);
            }
            return result;
        };
    }
}

void server::register_resource_template(
    const std::string& uri_template,
    const std::string& name,
    const std::string& mime_type,
    const std::string& description,
    resource_template_handler handler)
{
    std::lock_guard<std::mutex> lock(mutex_);
    resource_templates_.push_back({uri_template, name, mime_type, description, std::move(handler)});

    // Ensure resource read/list/template handlers are registered
    // (they may already exist if register_resource was called first)
    if (method_handlers_.find("resources/read") == method_handlers_.end()) {
        // Force registration by calling register_resource with a dummy,
        // or just register the read handler directly.
        method_handlers_["resources/read"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri") || !params["uri"].is_string()) {
                throw mcp_exception(error_code::invalid_params, "Missing or invalid 'uri' parameter");
            }
            std::string uri = params["uri"];
            auto it = resources_.find(uri);
            if (it != resources_.end()) {
                json contents = json::array();
                contents.push_back(it->second->read());
                return json{{"contents", contents}};
            }
            for (const auto& tmpl : resource_templates_) {
                std::map<std::string, std::string> uri_params;
                if (match_uri_template(tmpl.uri_template, uri, uri_params)) {
                    json result = tmpl.handler(uri, uri_params, session_id);
                    json contents = json::array();
                    contents.push_back(result);
                    return json{{"contents", contents}};
                }
            }
            throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
        };
    }

    if (method_handlers_.find("resources/templates/list") == method_handlers_.end()) {
        method_handlers_["resources/templates/list"] = [this](const json& params, const std::string& /*session_id*/) -> json {
            size_t start = 0;
            size_t page_size = 1000;
            if (params.contains("cursor") && params["cursor"].is_string()) {
                try { start = std::stoul(params["cursor"].get<std::string>()); } catch (...) {}
            }

            json templates_json = json::array();
            for (size_t i = start; i < resource_templates_.size() && templates_json.size() < page_size; i++) {
                const auto& tmpl = resource_templates_[i];
                templates_json.push_back({
                    {"uriTemplate", tmpl.uri_template},
                    {"name", tmpl.name},
                    {"description", tmpl.description},
                    {"mimeType", tmpl.mime_type}
                });
            }

            json result = {{"resourceTemplates", templates_json}};
            if (start + page_size < resource_templates_.size()) {
                result["nextCursor"] = std::to_string(start + page_size);
            }
            return result;
        };
    }
}

void server::register_tool(const tool& tool, tool_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_[tool.name] = std::make_pair(tool, handler);
    
    // Register methods for tool listing and calling
    if (method_handlers_.find("tools/list") == method_handlers_.end()) {
        method_handlers_["tools/list"] = [this](const json& params, const std::string& session_id) -> json {
            size_t start = 0;
            size_t page_size = 1000;
            if (params.contains("cursor") && params["cursor"].is_string()) {
                try { start = std::stoul(params["cursor"].get<std::string>()); } catch (...) {}
            }

            json tools_json = json::array();
            size_t idx = 0;
            for (const auto& [name, tool_pair] : tools_) {
                if (idx >= start && tools_json.size() < page_size) {
                    tools_json.push_back(tool_pair.first.to_json());
                }
                idx++;
            }

            json result = {{"tools", tools_json}};
            if (start + page_size < tools_.size()) {
                result["nextCursor"] = std::to_string(start + page_size);
            }
            return result;
        };
    }
    
    if (method_handlers_.find("tools/call") == method_handlers_.end()) {
        method_handlers_["tools/call"] = [this](const json& params, const std::string& session_id) -> json {
            // Spec 2025-11-25 (SEP-1303): tool input-validation failures are
            // returned as CallToolResult{ isError: true, ... }, not as
            // JSON-RPC -32602 protocol errors, so the model can self-correct.
            auto tool_error = [](const std::string& msg) {
                return json{
                    {"isError", true},
                    {"content", json::array({
                        {{"type", "text"}, {"text", msg}}
                    })}
                };
            };

            if (!params.contains("name") || !params["name"].is_string()) {
                return tool_error("Missing or invalid 'name' parameter");
            }

            std::string tool_name = params["name"];
            auto it = tools_.find(tool_name);
            if (it == tools_.end()) {
                return tool_error("Tool not found: " + tool_name);
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it_session = session_clients_.find(session_id);
                if (it_session != session_clients_.end()) {
                    ++it_session->second.tool_call_count;
                }
            }

            json tool_args = params.contains("arguments") ? params["arguments"] : json::object();

            // "arguments" is optional in the spec, so an explicit null means the
            // same thing as omitting the key. Clients routinely send it that way
            // for no-argument tools - our own sse_client does, since a default
            // constructed json is null - and schema validation would otherwise
            // reject every such call with "arguments must be of type 'object'".
            if (tool_args.is_null()) {
                tool_args = json::object();
            }

            if (tool_args.is_string()) {
                try {
                    tool_args = json::parse(tool_args.get<std::string>());
                } catch (const json::exception& e) {
                    return tool_error("Invalid JSON arguments: " + std::string(e.what()));
                }
            }

            std::string validation_error;
            if (!validate_tool_arguments(it->second.first.parameters_schema, tool_args, validation_error)) {
                return tool_error("Invalid tool arguments: " + validation_error);
            }

            json tool_result = {{"isError", false}};
            try {
                tool_result["content"] = it->second.second(tool_args, session_id);
            } catch (const mcp_exception& e) {
                tool_result["isError"] = true;
                tool_result["content"] = json::array({
                    {{"type", "text"},
                     {"text", mcp_exception_message(e, expose_error_details_,
                                                    "Tool execution failed")}}
                });
            } catch (const std::exception& e) {
                LOG_ERROR("Tool handler exception for ", tool_name, ": ", e.what());
                tool_result["isError"] = true;
                tool_result["content"] = json::array({
                    {{"type", "text"},
                     {"text", expose_error_details_ ? e.what() : "Tool execution failed"}}
                });
            }
            return tool_result;
        };
    }
}

void server::register_prompt(const prompt& prompt, prompt_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    prompts_[prompt.name] = std::make_pair(prompt, handler);

    if (method_handlers_.find("prompts/list") == method_handlers_.end()) {
        method_handlers_["prompts/list"] = [this](const json& params, const std::string& session_id) -> json {
            size_t start = 0;
            size_t page_size = 1000;
            if (params.contains("cursor") && params["cursor"].is_string()) {
                try { start = std::stoul(params["cursor"].get<std::string>()); } catch (...) {}
            }

            json prompts_json = json::array();
            size_t idx = 0;
            for (const auto& [name, prompt_pair] : prompts_) {
                if (idx >= start && prompts_json.size() < page_size) {
                    prompts_json.push_back(prompt_pair.first.to_json());
                }
                idx++;
            }

            json result = {{"prompts", prompts_json}};
            if (start + page_size < prompts_.size()) {
                result["nextCursor"] = std::to_string(start + page_size);
            }
            return result;
        };
    }

    if (method_handlers_.find("prompts/get") == method_handlers_.end()) {
        method_handlers_["prompts/get"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("name") || !params["name"].is_string()) {
                throw mcp_exception(error_code::invalid_params, "Missing or invalid 'name' parameter");
            }

            std::string prompt_name = params["name"];
            auto it = prompts_.find(prompt_name);
            if (it == prompts_.end()) {
                throw mcp_exception(error_code::invalid_params, "Prompt not found: " + prompt_name);
            }

            json arguments = params.contains("arguments") ? params["arguments"] : json::object();
            json result = it->second.second(arguments, session_id);

            // Ensure the result contains description
            if (!result.contains("description")) {
                result["description"] = it->second.first.description;
            }
            return result;
        };
    }
}

void server::register_session_cleanup(const std::string& key, session_cleanup_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_cleanup_handler_[key] = handler;
}

void server::register_session_open(const std::string& key, session_open_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_open_handler_[key] = handler;
}

std::vector<tool> server::get_tools() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<tool> tools;
    
    for (const auto& [name, tool_pair] : tools_) {
        tools.push_back(tool_pair.first);
    }
    
    return tools;
}

void server::set_auth_handler(auth_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    auth_handler_ = std::move(handler);
    detailed_auth_handler_ = {};
}

void server::touch_session(const std::string& session_id, const std::string& remote_addr) {
    if (session_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = session_clients_.find(session_id);
    if (it == session_clients_.end()) {
        return; // not initialized yet; identity arrives with initialize
    }
    it->second.last_seen_unix = unix_now();
    if (!remote_addr.empty()) {
        it->second.remote_addr = remote_addr;
    }
}

std::vector<session_info> server::get_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<session_info> out;
    out.reserve(session_clients_.size());
    for (const auto& entry : session_clients_) {
        out.push_back(entry.second);
    }
    return out;
}

void server::set_detailed_auth_handler(detailed_auth_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    detailed_auth_handler_ = std::move(handler);
    auth_handler_ = {};
}

void server::handle_sse(const httplib::Request& req, httplib::Response& res) {
    if (!origin_is_allowed(req)) {
        res.status = 403;
        res.set_content("{\"error\":\"Forbidden origin\"}", "application/json");
        return;
    }
    set_cors_headers(req, res, "GET, OPTIONS");

    const auto authorization = authorize_request(req, "");
    if (authorization.status != auth_status::authorized) {
        reject_authorization(res, authorization);
        return;
    }

    // Enforce session limit
    if (max_sessions_ > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_dispatchers_.size() >= max_sessions_) {
            LOG_WARNING("Max sessions reached (", max_sessions_, "), rejecting SSE connection");
            res.status = 503;
            res.set_content("{\"error\":\"Too many sessions\"}", "application/json");
            return;
        }
    }

    std::string session_id = generate_session_id();
    std::string session_uri = msg_endpoint_ + "?session_id=" + session_id;
    
    // Setup SSE response headers
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    
    // Create session-specific event dispatcher
    auto session_dispatcher = std::make_shared<event_dispatcher>();
    
    // Initialize activity time
    session_dispatcher->update_activity();
    
    // Add session dispatcher to mapping table
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_dispatchers_[session_id] = session_dispatcher;
    }
    
    // Create session thread
    auto thread = std::make_unique<std::thread>([this, res, session_id, session_uri, session_dispatcher]() {
        try {
            // Send initial session URI
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::stringstream ss;
            ss << "event: endpoint\r\ndata: " << session_uri << "\r\n\r\n";
            session_dispatcher->send_event(ss.str());
            
            // Update activity time (after sending message)
            session_dispatcher->update_activity();
            
            // Periodic SSE keepalives. Use bare ":" comments rather than a
            // custom "event: heartbeat" so spec-compliant clients ignore them
            // outright; some MCP clients treat unknown event types as
            // protocol violations and tear down the stream, which we saw on
            // the Aurora device producing ~30-55s session lifetimes.
            //
            // Uses an interruptible wait so server::stop() (which calls
            // dispatcher close()) can wake this thread immediately —
            // previously a naked sleep_for(5s) meant stop() had to either
            // wait up to 5s or detach the thread (hazardous: detached thread
            // outlives server and crashes on use-after-free).
            while (running_ && !session_dispatcher->is_closed()) {
                auto timeout = std::chrono::seconds(5) +
                               std::chrono::milliseconds(rand() % 500);
                if (session_dispatcher->wait_for_close(timeout)) {
                    break; // Dispatcher closed — exit cleanly
                }

                if (session_dispatcher->is_closed() || !running_) {
                    break;
                }

                try {
                    bool sent = session_dispatcher->send_event(":\n\n");
                    if (!sent) {
                        LOG_WARNING("Failed to send keepalive, client may have closed connection: ", session_id);
                        break;
                    }
                    session_dispatcher->update_activity();
                } catch (const std::exception& e) {
                    LOG_ERROR("Failed to send keepalive: ", e.what());
                    break;
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("SSE session thread exception: ", session_id, ", ", e.what());
        }
        
        close_session(session_id);
    });
    
    // Store thread
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sse_threads_[session_id] = std::move(thread);
    }
    
    // Setup chunked content provider
    res.set_chunked_content_provider("text/event-stream", [this, session_id, session_dispatcher](size_t /* offset */, httplib::DataSink& sink) {
        try {
            // Check if session is closed - directly get status from dispatcher, reduce lock contention
            if (session_dispatcher->is_closed()) {
                return false;
            }
            
            // Update activity time (received request)
            session_dispatcher->update_activity();
            
            // Wait for event
            bool result = session_dispatcher->wait_event(&sink);
            if (!result) {
                LOG_WARNING("Failed to wait for event, closing connection: ", session_id);
                
                close_session(session_id);
                
                return false;
            }
            
            // Update activity time (successfully received message)
            session_dispatcher->update_activity();

            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("SSE content provider exception: ", e.what());
            
            close_session(session_id);
            
            return false;
        }
    });
}

void server::handle_jsonrpc(const httplib::Request& req, httplib::Response& res) {
    if (!origin_is_allowed(req)) {
        res.status = 403;
        res.set_content("{\"error\":\"Forbidden origin\"}", "application/json");
        return;
    }
    auto sid_it = req.params.find("session_id");
    std::string session_id = sid_it != req.params.end() ? sid_it->second : "";

    // Setup response headers
    res.set_header("Content-Type", "application/json");
    set_cors_headers(req, res, "POST, OPTIONS");

    const auto authorization = authorize_request(req, session_id);
    if (authorization.status != auth_status::authorized) {
        reject_authorization(res, authorization);
        return;
    }

    // Handle OPTIONS request (CORS pre-flight)
    if (req.method == "OPTIONS") {
        res.status = 204; // No Content
        return;
    }

    // Update session activity time
    if (!session_id.empty()) {
        std::shared_ptr<event_dispatcher> dispatcher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto disp_it = session_dispatchers_.find(session_id);
            if (disp_it != session_dispatchers_.end()) {
                dispatcher = disp_it->second;
            }
        }
        
        if (dispatcher) {
            dispatcher->update_activity();
        }
    }
    
    // Parse request
    json req_json;
    try {
        req_json = json::parse(req.body);
    } catch (const json::exception& e) {
        LOG_ERROR("Failed to parse JSON request: ", e.what());
        res.status = 400;
        res.set_content("{\"error\":\"Invalid JSON\"}", "application/json");
        return;
    }
    
    // Check if session exists
    std::shared_ptr<event_dispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto disp_it = session_dispatchers_.find(session_id);
        if (disp_it == session_dispatchers_.end()) {
            // Handle ping request
            if (req_json["method"] == "ping") {
                res.status = 202;
                res.set_content("Accepted", "text/plain");
                return;
            }
            LOG_ERROR("Session not found: ", session_id);
            res.status = 404;
            res.set_content("{\"error\":\"Session not found\"}", "application/json");
            return;
        }
        dispatcher = disp_it->second;
    }
    
    // Create request object
    request mcp_req;
    try {
        mcp_req.jsonrpc = req_json["jsonrpc"].get<std::string>();
        if (req_json.contains("id") && !req_json["id"].is_null()) {
            mcp_req.id = req_json["id"];
        }
        mcp_req.method = req_json["method"].get<std::string>();
        if (req_json.contains("params")) {
            mcp_req.params = req_json["params"];
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create request object: ", e.what());
        res.status = 400;
        res.set_content("{\"error\":\"Invalid request format\"}", "application/json");
        return;
    }
    
    // If it is a notification (no ID), process it directly and return 202 status code
    if (mcp_req.is_notification()) {
        // Process it asynchronously in the thread pool
        try {
            thread_pool_.enqueue([this, mcp_req, session_id]() {
                process_request(mcp_req, session_id);
            });
        } catch (const std::exception& e) {
            LOG_WARNING("Failed to enqueue notification: ", e.what());
            res.status = 503;
            res.set_content("{\"error\":\"Server busy\"}", "application/json");
            return;
        }
        
        // Return 202 Accepted
        res.status = 202;
        res.set_content("Accepted", "text/plain");
        return;
    }
    
    // For requests with ID, process it asynchronously in the thread pool and return the result via SSE
    try {
        thread_pool_.enqueue([this, mcp_req, session_id, dispatcher]() {
            // Process the request
            json response_json = process_request(mcp_req, session_id);

            // Send response via SSE
            std::stringstream ss;
            ss << "event: message\r\ndata: " << response_json.dump() << "\r\n\r\n";
            bool result = dispatcher->send_event(ss.str());

            if (!result) {
                LOG_ERROR("Failed to send response via SSE: session_id=", session_id);
            }
        });
    } catch (const std::exception& e) {
        LOG_WARNING("Failed to enqueue request: ", e.what());
        res.status = 503;
        res.set_content("{\"error\":\"Server busy\"}", "application/json");
        return;
    }
    
    // Return 202 Accepted
    res.status = 202;
    res.set_content("Accepted", "text/plain");
}

// ---------------------------------------------------------------------------
// Streamable HTTP transport (2025-03-26 spec)
// ---------------------------------------------------------------------------

bool server::origin_is_allowed(const httplib::Request& req) const {
    if (allowed_origins_.empty()) {
        return true;  // unset = no check
    }
    std::string origin = req.get_header_value("Origin");
    if (origin.empty()) {
        return true;  // browsers omit for same-origin / non-browser clients
    }
    return std::find(allowed_origins_.begin(), allowed_origins_.end(), origin)
           != allowed_origins_.end();
}

void server::set_cors_headers(const httplib::Request& req,
                              httplib::Response& res,
                              const std::string& methods) const {
    const std::string origin = req.get_header_value("Origin");
    if (allowed_origins_.empty()) {
        res.set_header("Access-Control-Allow-Origin", "*");
    } else if (!origin.empty() && origin_is_allowed(req)) {
        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Vary", "Origin");
    }

    res.set_header("Access-Control-Allow-Methods", methods);
    res.set_header("Access-Control-Allow-Headers",
                   "Authorization, Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version");
    res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id, MCP-Protocol-Version");
}

auth_result server::authorize_request(const httplib::Request& req,
                                      const std::string& session_id) const {
    auth_handler handler;
    detailed_auth_handler detailed_handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = auth_handler_;
        detailed_handler = detailed_auth_handler_;
    }

    if (!handler && !detailed_handler) {
        return auth_result::allow();
    }

    const std::string auth = req.get_header_value("Authorization");
    constexpr const char* bearer_prefix = "Bearer ";
    if (!starts_with_case_insensitive(auth, bearer_prefix)) {
        return {auth_status::unauthorized, "", ""};
    }

    std::string token = auth.substr(std::string(bearer_prefix).size());
    if (token.empty()) {
        return {auth_status::unauthorized, "", ""};
    }

    try {
        if (detailed_handler) {
            return detailed_handler(token, session_id);
        }
        if (!handler(token, session_id)) {
            return auth_result::reject();
        }
    } catch (const std::exception& e) {
        LOG_WARNING("Auth handler rejected request with exception: ", e.what());
        return auth_result::reject();
    } catch (...) {
        LOG_WARNING("Auth handler rejected request with unknown exception");
        return auth_result::reject();
    }

    return auth_result::allow();
}

void server::reject_authorization(httplib::Response& res,
                                  const auth_result& result) const {
    const bool forbidden = result.status == auth_status::forbidden;
    res.status = forbidden ? 403 : 401;

    std::string challenge = "Bearer";
    if (!result.error.empty()) {
        challenge += " error=" + quoted_auth_parameter(result.error);
    }
    if (!result.scope.empty()) {
        challenge += " scope=" + quoted_auth_parameter(result.scope);
    }
    if (!auth_resource_metadata_url_.empty()) {
        challenge += " resource_metadata=" +
                     quoted_auth_parameter(auth_resource_metadata_url_);
    }
    res.set_header("WWW-Authenticate", challenge);
    res.set_content(
        response::create_error(nullptr, error_code::invalid_request,
                               forbidden ? "Forbidden" : "Unauthorized")
            .to_json().dump(),
        "application/json");
}

std::pair<int, std::string>
server::validate_protocol_version_header(const httplib::Request& req,
                                         const std::string& session_id) const {
    std::string header = req.get_header_value("MCP-Protocol-Version");
    if (header.empty()) {
        // Spec compat: missing header implies 2025-03-26.
        return {200, ""};
    }
    if (!is_supported_version(header)) {
        return {400, "Unsupported MCP-Protocol-Version: " + header};
    }
    std::string negotiated = session_protocol_version(session_id);
    if (!negotiated.empty() && header != negotiated) {
        return {400,
            "MCP-Protocol-Version header (" + header +
            ") does not match negotiated session version (" + negotiated + ")"};
    }
    return {200, ""};
}

request server::parse_jsonrpc_message(const json& j) const {
    if (!j.is_object()) {
        throw mcp_exception(error_code::invalid_request, "JSON-RPC message must be an object");
    }

    request req;
    if (!j.contains("jsonrpc") || !j["jsonrpc"].is_string() ||
        j["jsonrpc"].get<std::string>() != "2.0") {
        throw mcp_exception(error_code::invalid_request, "Expected jsonrpc to be \"2.0\"");
    }
    req.jsonrpc = "2.0";

    if (j.contains("id") && !j["id"].is_null()) {
        if (!is_valid_jsonrpc_id(j["id"])) {
            throw mcp_exception(error_code::invalid_request,
                                "JSON-RPC id must be a string, integer, or null");
        }
        req.id = j["id"];
    }

    if (!j.contains("method") || !j["method"].is_string() ||
        j["method"].get<std::string>().empty()) {
        throw mcp_exception(error_code::invalid_request,
                            "Expected non-empty string for JSON-RPC method");
    }
    req.method = j["method"].get<std::string>();

    if (j.contains("params")) {
        if (!j["params"].is_object()) {
            throw mcp_exception(error_code::invalid_params,
                                "Expected object for JSON-RPC params");
        }
        req.params = j["params"];
    }
    return req;
}

void server::handle_mcp_post(const httplib::Request& req, httplib::Response& res) {
    if (!origin_is_allowed(req)) {
        res.status = 403;
        res.set_content("{\"error\":\"Forbidden origin\"}", "application/json");
        return;
    }

    set_cors_headers(req, res, "GET, POST, DELETE, OPTIONS");

    // Get or create session
    std::string session_id = req.get_header_value("Mcp-Session-Id");
    touch_session(session_id, req.remote_addr);

    const auto authorization = authorize_request(req, session_id);
    if (authorization.status != auth_status::authorized) {
        reject_authorization(res, authorization);
        return;
    }

    // Reflect the protocol version of this exchange on every response.
    {
        std::string ver = !session_id.empty() ? session_protocol_version(session_id) : "";
        if (ver.empty()) ver = LATEST_MCP_VERSION;
        res.set_header("MCP-Protocol-Version", ver);
    }

    // Parse JSON body
    json body;
    try {
        body = json::parse(req.body);
    } catch (const json::exception& e) {
        LOG_ERROR("Failed to parse JSON: ", e.what());
        res.status = 400;
        res.set_content(
            response::create_error(nullptr, error_code::parse_error, "Invalid JSON").to_json().dump(),
            "application/json");
        return;
    }

    if (!body.is_object()) {
        set_jsonrpc_error(res, 400, nullptr, error_code::invalid_request,
                          "JSON-RPC message must be an object");
        return;
    }

    // Check if this is an initialize request (no session needed)
    bool is_initialize = false;
    if (body.contains("method") && body["method"].is_string() && body["method"] == "initialize") {
        is_initialize = true;
    }

    // Reject re-initialization on an existing session
    if (is_initialize && !session_id.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_dispatchers_.find(session_id) != session_dispatchers_.end()) {
            res.status = 400;
            res.set_content("{\"error\":\"Session already initialized. Delete and re-create.\"}",
                            "application/json");
            return;
        }
    }

    // Validate session for non-initialize requests
    if (!is_initialize) {
        if (session_id.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing Mcp-Session-Id header\"}", "application/json");
            return;
        }
        std::shared_ptr<event_dispatcher> dispatcher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto dispatcher_it = session_dispatchers_.find(session_id);
            if (dispatcher_it == session_dispatchers_.end()) {
                // Session expired or invalid — client must re-initialize
                res.status = 404;
                res.set_content("{\"error\":\"Session not found\"}", "application/json");
                return;
            }
            dispatcher = dispatcher_it->second;
        }
        auto [vstatus, vmsg] = validate_protocol_version_header(req, session_id);
        if (vstatus != 200) {
            res.status = vstatus;
            res.set_content(
                response::create_error(nullptr, error_code::invalid_request, vmsg)
                    .to_json().dump(),
                "application/json");
            return;
        }
        dispatcher->update_activity();
    }

    // JSON-RPC responses sent by clients are accepted and ignored.
    if (!body.contains("method") && body.contains("id") &&
        (body.contains("result") || body.contains("error"))) {
        if (!session_id.empty()) {
            res.status = 202;
            return;
        }
        set_jsonrpc_error(res, 400, request_id_or_null(body), error_code::invalid_request,
                          "Missing Mcp-Session-Id header");
        return;
    }

    request mcp_req;
    try {
        mcp_req = parse_jsonrpc_message(body);
    } catch (const mcp_exception& e) {
        set_jsonrpc_error(res, 400, request_id_or_null(body), e.code(), e.what());
        return;
    } catch (const std::exception& e) {
        LOG_WARNING("Invalid JSON-RPC request: ", e.what());
        set_jsonrpc_error(res, 400, request_id_or_null(body), error_code::invalid_request,
                          "Invalid JSON-RPC request");
        return;
    }

    // Notifications (no id, or id=null): fire and forget.
    bool has_request_id = body.contains("id") && !body["id"].is_null();
    if (!has_request_id) {
        if (!session_id.empty()) {
            process_request(mcp_req, session_id);
        }
        res.status = 202;
        return;
    }

    // Has requests — process and decide response format
    // For initialize: create session, return inline JSON with Mcp-Session-Id header
    if (is_initialize) {
        // Enforce session limit
        if (max_sessions_ > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_dispatchers_.size() >= max_sessions_) {
                res.status = 503;
                res.set_content("{\"error\":\"Too many sessions\"}", "application/json");
                return;
            }
        }

        session_id = generate_session_id();

        // Create session dispatcher for server-push via GET
        auto session_dispatcher = std::make_shared<event_dispatcher>();
        session_dispatcher->update_activity();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_dispatchers_[session_id] = session_dispatcher;
        }

        json result = handle_initialize(mcp_req, session_id);

        res.set_header("Mcp-Session-Id", session_id);
        // Override the placeholder set at the top of the handler now that we
        // know what version was negotiated.
        std::string negotiated = session_protocol_version(session_id);
        if (!negotiated.empty()) {
            res.set_header("MCP-Protocol-Version", negotiated);
        }
        res.set_header("Content-Type", "application/json");
        res.set_content(result.dump(), "application/json");
        return;
    }

    // Non-initialize request with an id: process synchronously and return inline JSON.
    json result = process_request(mcp_req, session_id);
    res.set_header("Content-Type", "application/json");
    res.set_content(result.dump(), "application/json");
}

void server::handle_mcp_get(const httplib::Request& req, httplib::Response& res) {
    if (!origin_is_allowed(req)) {
        res.status = 403;
        res.set_content("{\"error\":\"Forbidden origin\"}", "application/json");
        return;
    }
    set_cors_headers(req, res, "GET, OPTIONS");

    std::string session_id = req.get_header_value("Mcp-Session-Id");
    touch_session(session_id, req.remote_addr);
    const auto authorization = authorize_request(req, session_id);
    if (authorization.status != auth_status::authorized) {
        reject_authorization(res, authorization);
        return;
    }

    {
        std::string ver = !session_id.empty() ? session_protocol_version(session_id) : "";
        if (ver.empty()) ver = LATEST_MCP_VERSION;
        res.set_header("MCP-Protocol-Version", ver);
    }
    if (session_id.empty()) {
        res.status = 400;
        res.set_content("{\"error\":\"Missing Mcp-Session-Id header\"}", "application/json");
        return;
    }

    // Validate session
    std::shared_ptr<event_dispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_dispatchers_.find(session_id);
        if (it == session_dispatchers_.end()) {
            res.status = 404;
            res.set_content("{\"error\":\"Session not found\"}", "application/json");
            return;
        }
        dispatcher = it->second;
    }

    if (!is_session_initialized(session_id)) {
        res.status = 400;
        res.set_content("{\"error\":\"Session not initialized\"}", "application/json");
        return;
    }

    auto [vstatus, vmsg] = validate_protocol_version_header(req, session_id);
    if (vstatus != 200) {
        res.status = vstatus;
        res.set_content(
            response::create_error(nullptr, error_code::invalid_request, vmsg)
                .to_json().dump(),
            "application/json");
        return;
    }

    // Open SSE stream for server-initiated notifications
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");

    // Emit an initial SSE comment immediately so clients know the stream is
    // established before the first keepalive interval elapses.
    auto sent_initial_comment = std::make_shared<std::atomic<bool>>(false);

    // Use chunked content provider — same pattern as legacy SSE
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, session_id, dispatcher, sent_initial_comment](size_t, httplib::DataSink& sink) {
            try {
                if (dispatcher->is_closed() || !running_) {
                    return false;
                }

                if (!sent_initial_comment->exchange(true, std::memory_order_acq_rel)) {
                    if (!sink.write(":\n\n", 3)) {
                        return false;
                    }
                }

                dispatcher->update_activity();
                bool result = dispatcher->wait_event(&sink);
                if (!result) {
                    return false;
                }
                dispatcher->update_activity();
                return true;
            } catch (...) {
                return false;
            }
        });
}

void server::handle_mcp_delete(const httplib::Request& req, httplib::Response& res) {
    if (!origin_is_allowed(req)) {
        res.status = 403;
        res.set_content("{\"error\":\"Forbidden origin\"}", "application/json");
        return;
    }
    set_cors_headers(req, res, "DELETE, OPTIONS");

    std::string session_id = req.get_header_value("Mcp-Session-Id");
    touch_session(session_id, req.remote_addr);
    const auto authorization = authorize_request(req, session_id);
    if (authorization.status != auth_status::authorized) {
        reject_authorization(res, authorization);
        return;
    }

    {
        std::string ver = !session_id.empty() ? session_protocol_version(session_id) : "";
        if (ver.empty()) ver = LATEST_MCP_VERSION;
        res.set_header("MCP-Protocol-Version", ver);
    }
    if (session_id.empty()) {
        res.status = 400;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_dispatchers_.find(session_id) == session_dispatchers_.end()) {
            res.status = 404;
            return;
        }
    }

    auto [vstatus, vmsg] = validate_protocol_version_header(req, session_id);
    if (vstatus != 200) {
        res.status = vstatus;
        res.set_content(
            response::create_error(nullptr, error_code::invalid_request, vmsg)
                .to_json().dump(),
            "application/json");
        return;
    }

    close_session(session_id);
    res.status = 200;
}

json server::process_request(const request& req, const std::string& session_id) {
    // Check if it is a notification
    if (req.is_notification()) {
        if (req.method == "notifications/initialized") {
            set_session_initialized(session_id, true);
        } else if (req.method == "notifications/cancelled") {
            // Track cancelled request IDs
            if (req.params.contains("requestId")) {
                std::string rid = req.params["requestId"].dump();
                std::lock_guard<std::mutex> lock(mutex_);
                cancelled_requests_[session_id].insert(rid);
            }
        }

        // Dispatch to registered notification handlers
        notification_handler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = notification_handlers_.find(req.method);
            if (it != notification_handlers_.end()) {
                handler = it->second;
            }
        }
        if (handler) {
            try {
                handler(req.params, session_id);
            } catch (const std::exception& e) {
                LOG_ERROR("Notification handler exception for ", req.method, ": ", e.what());
            } catch (...) {
                LOG_ERROR("Unknown notification handler exception for ", req.method);
            }
        }

        return json::object();
    }
    
    // Process method call
    try {
        LOG_INFO("Processing method call: ", req.method);
        
        // Special case: initialization
        if (req.method == "initialize") {
            return handle_initialize(req, session_id);
        } else if (req.method == "ping") {
            return response::create_success(req.id, json::object()).to_json();
        } else if (req.method == "logging/setLevel") {
            if (!req.params.contains("level") || !req.params["level"].is_string()) {
                return response::create_error(req.id, error_code::invalid_params,
                    "Missing or invalid 'level' parameter").to_json();
            }
            std::string level = req.params["level"].get<std::string>();
            if (level != "debug" && level != "info" && level != "notice" &&
                level != "warning" && level != "error" && level != "critical" &&
                level != "alert" && level != "emergency") {
                return response::create_error(req.id, error_code::invalid_params,
                    "Invalid log level").to_json();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                session_log_levels_[session_id] = level;
            }
            LOG_INFO("Session ", session_id, " set log level to: ", level);
            return response::create_success(req.id, json::object()).to_json();
        }

        if (!is_session_initialized(session_id)) {
            LOG_WARNING("Session not initialized: ", session_id);
            return response::create_error(
                req.id,
                error_code::invalid_request,
                "Session not initialized"
            ).to_json();
        }
        
        // Find registered method handler
        method_handler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = method_handlers_.find(req.method);
            if (it != method_handlers_.end()) {
                handler = it->second;
            }
        }
        
        if (handler) {
            // Call handler
            LOG_INFO("Calling method handler: ", req.method);            
            json result = handler(req.params, session_id);
            
            // Create success response
            LOG_INFO("Method call successful: ", req.method);
            return response::create_success(req.id, result).to_json();
        }
        
        // Method not found
        LOG_WARNING("Method not found: ", req.method);
        return response::create_error(
            req.id,
            error_code::method_not_found,
            "Method not found: " + req.method
        ).to_json();
    } catch (const mcp_exception& e) {
        // MCP exception
        LOG_ERROR("MCP exception: ", e.what(), ", code: ", static_cast<int>(e.code()));
        return response::create_error(
            req.id,
            e.code(),
            mcp_exception_message(e, expose_error_details_, "Internal error")
        ).to_json();
    } catch (const std::exception& e) {
        // Other exceptions
        LOG_ERROR("Exception while processing request: ", e.what());
        return response::create_error(
            req.id,
            error_code::internal_error,
            expose_error_details_ ? "Internal error: " + std::string(e.what()) : "Internal error"
        ).to_json();
    } catch (...) {
        // Unknown exception
        LOG_ERROR("Unknown exception while processing request");
        return response::create_error(
            req.id,
            error_code::internal_error,
            "Unknown internal error"
        ).to_json();
    }
}

json server::handle_initialize(const request& req, const std::string& session_id) {
    const json& params = req.params;

    // Version negotiation
    if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string()) {
        LOG_ERROR("Missing or invalid protocolVersion parameter");
        return response::create_error(
            req.id, 
            error_code::invalid_params, 
            "Expected string for 'protocolVersion' parameter"
        ).to_json();
    }

    std::string requested_version = params["protocolVersion"].get<std::string>();
    LOG_INFO("Client requested protocol version: ", requested_version);

    // Spec: if the client requests a version we support, return that version;
    // otherwise return our latest supported version and let the client decide
    // whether to disconnect.
    std::string negotiated_version;
    if (is_supported_version(requested_version)) {
        negotiated_version = requested_version;
    } else {
        LOG_WARNING("Client requested unsupported version ", requested_version,
                    ", falling back to latest ", LATEST_MCP_VERSION);
        negotiated_version = LATEST_MCP_VERSION;
    }

    // Extract client info
    std::string client_name = "UnknownClient";
    std::string client_version = "UnknownVersion";
    
    if (params.contains("clientInfo")) {
        if (params["clientInfo"].contains("name")) {
            client_name = params["clientInfo"]["name"];
        }
        if (params["clientInfo"].contains("version")) {
            client_version = params["clientInfo"]["version"];
        }
    }
    
    // Log connection
    LOG_INFO("Client connected: ", client_name, " ", client_version);
    
    // Return server info and capabilities
    json server_info = {
        {"name", name_},
        {"version", version_}
    };

    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_protocol_versions_[session_id] = negotiated_version;

        // clientInfo arrives once, here. Keeping it is what lets an operator
        // tell their own tooling apart from a client they did not start.
        auto& info = session_clients_[session_id];
        info.session_id = session_id;
        info.client_name = client_name;
        info.client_version = client_version;
        info.protocol_version = negotiated_version;
        info.connected_at_unix = unix_now();
        info.last_seen_unix = info.connected_at_unix;
    }

    // Announce the session now that session_clients_ holds it, so a handler
    // that calls get_sessions() sees the client it is being told about. Copied
    // under the lock and invoked outside it: a handler is host code and may do
    // arbitrary work, including calling back into this server.
    {
        std::map<std::string, session_open_handler> open_handlers_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_handlers_copy = session_open_handler_;
        }
        for (const auto& entry : open_handlers_copy) {
            if (entry.second) {
                entry.second(session_id);
            }
        }
    }

    json result = {
        {"protocolVersion", negotiated_version},
        {"capabilities", capabilities_},
        {"serverInfo", server_info}
    };

    if (!instructions_.empty()) {
        result["instructions"] = instructions_;
    }

    LOG_INFO("Initialization successful, waiting for notifications/initialized notification");
    
    return response::create_success(req.id, result).to_json();
}

void server::send_jsonrpc(const std::string& session_id, const json& message) {
    // Check if session ID is valid
    if (session_id.empty()) {
        LOG_WARNING("Cannot send message to empty session_id");
        return;
    }

    // Get session dispatcher
    std::shared_ptr<event_dispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_dispatchers_.find(session_id);
        if (it == session_dispatchers_.end()) {
            LOG_ERROR("Session not found: ", session_id);
            return;
        }
        dispatcher = it->second;
    }
    
    // Confirm dispatcher is still valid
    if (!dispatcher || dispatcher->is_closed()) {
        LOG_WARNING("Cannot send to closed session: ", session_id);
        return;
    }
    
    // Send message
    std::stringstream ss;
    ss << "event: message\r\ndata: " << message.dump() << "\r\n\r\n";
    bool result = dispatcher->send_event(ss.str());
    
    if (!result) {
        LOG_ERROR("Failed to send message to session: ", session_id);
    }
}

void server::send_request(const std::string& session_id, const request& req) {
    send_jsonrpc(session_id, req.to_json());
}

void server::broadcast_notification(const request& notification) {
    std::vector<std::string> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [sid, initialized] : session_initialized_) {
            if (initialized) {
                sessions.push_back(sid);
            }
        }
    }
    for (const auto& sid : sessions) {
        try {
            send_jsonrpc(sid, notification.to_json());
        } catch (...) {
            // Best-effort delivery; don't fail if one session is broken
        }
    }
}

// Log level ordering per MCP spec (syslog severity)
static int log_level_severity(const std::string& level) {
    if (level == "emergency") return 0;
    if (level == "alert") return 1;
    if (level == "critical") return 2;
    if (level == "error") return 3;
    if (level == "warning") return 4;
    if (level == "notice") return 5;
    if (level == "info") return 6;
    if (level == "debug") return 7;
    return 4; // default to warning
}

void server::send_log(const std::string& session_id, const std::string& level,
                      const std::string& logger, const json& data) {
    // Check if session accepts this log level
    std::string session_level;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_log_levels_.find(session_id);
        session_level = (it != session_log_levels_.end()) ? it->second : "warning";
    }
    if (log_level_severity(level) > log_level_severity(session_level)) {
        return; // Level too verbose for this session
    }

    json params = {{"level", level}, {"logger", logger}, {"data", data}};
    auto notif = request::create_notification("message");
    notif.params = params;
    send_jsonrpc(session_id, notif.to_json());
}

void server::broadcast_log(const std::string& level, const std::string& logger, const json& data) {
    std::vector<std::string> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [sid, initialized] : session_initialized_) {
            if (initialized) sessions.push_back(sid);
        }
    }
    for (const auto& sid : sessions) {
        try {
            send_log(sid, level, logger, data);
        } catch (...) {}
    }
}

std::vector<std::string> server::get_active_sessions() const {
    std::vector<std::string> sessions;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [sid, initialized] : session_initialized_) {
        if (initialized) {
            sessions.push_back(sid);
        }
    }
    return sessions;
}

void server::send_progress(const std::string& session_id, const json& progress_token,
                           double progress, double total, const std::string& message) {
    json params = {
        {"progressToken", progress_token},
        {"progress", progress}
    };
    if (total >= 0) {
        params["total"] = total;
    }
    if (!message.empty()) {
        params["message"] = message;
    }
    auto notif = request::create_notification("progress");
    notif.params = params;
    // Fix the method — create_notification prepends "notifications/"
    send_jsonrpc(session_id, notif.to_json());
}

bool server::is_cancelled(const json& request_id, const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cancelled_requests_.find(session_id);
    if (it == cancelled_requests_.end()) return false;
    return it->second.count(request_id.dump()) > 0;
}

std::string server::session_protocol_version(const std::string& session_id) const {
    if (session_id.empty()) {
        return "";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = session_protocol_versions_.find(session_id);
    return it != session_protocol_versions_.end() ? it->second : "";
}

bool server::is_session_initialized(const std::string& session_id) const {
    // Check if session ID is valid
    if (session_id.empty()) {
        return false;
    }
    
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_initialized_.find(session_id);
        return (it != session_initialized_.end() && it->second);
    } catch (const std::exception& e) {
        LOG_ERROR("Exception checking if session is initialized: ", e.what());
        return false;
    }
}

void server::set_session_initialized(const std::string& session_id, bool initialized) {
    // Check if session ID is valid
    if (session_id.empty()) {
        LOG_WARNING("Cannot set initialization state for empty session_id");
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(mutex_);
        // Check if session still exists (either SSE or HTTP mode)
        auto it = session_dispatchers_.find(session_id);
        bool has_dispatcher = (it != session_dispatchers_.end());

        // For HTTP mode, we also track initialization in session_initialized_ map
        // So we allow setting initialized state even without a dispatcher for HTTP sessions
        if (!has_dispatcher) {
            LOG_DEBUG("Setting initialization state for HTTP session: ", session_id);
        }

        session_initialized_[session_id] = initialized;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception setting session initialization state: ", e.what());
    }
}

std::string server::generate_session_id() const {
    std::array<unsigned char, 16> bytes{};
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(bytes.data(), bytes.size());
#else
    std::random_device rd;
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(rd() & 0xff);
    }
#endif

    // RFC 4122 version 4 UUID bits.
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            ss << "-";
        }
        ss << std::setw(2) << static_cast<int>(bytes[i]);
    }

    return ss.str();
}

void server::start_maintenance_thread() {
    if (session_timeout_ == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(maintenance_mutex_);
    if (maintenance_thread_run_) {
        return;
    }

    maintenance_thread_run_ = true;
    maintenance_thread_ = std::make_unique<std::thread>([this]() {
        const auto check_interval =
            std::chrono::seconds(std::min(session_timeout_, 10u));
        while (true) {
            // Check at least once per configured timeout, capped at 10 seconds.
            std::unique_lock<std::mutex> lock(maintenance_mutex_);
            auto should_exit = maintenance_cond_.wait_for(lock, check_interval, [this] {
                return !maintenance_thread_run_;
            });
            if (should_exit) {
                LOG_INFO("Maintenance thread exiting");
                return;
            }
            lock.unlock();

            try {
                check_inactive_sessions();
            } catch (const std::exception& e) {
                LOG_ERROR("Exception in maintenance thread: ", e.what());
            } catch (...) {
                LOG_ERROR("Unknown exception in maintenance thread");
            }
        }
    });
}

void server::stop_maintenance_thread() {
    std::unique_ptr<std::thread> thread_to_join;
    {
        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        maintenance_thread_run_ = false;
        thread_to_join = std::move(maintenance_thread_);
    }

    maintenance_cond_.notify_one();

    if (thread_to_join && thread_to_join->joinable()) {
        try {
            thread_to_join->join();
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to join maintenance thread: ", e.what());
        }
    }
}

void server::check_inactive_sessions() {
    if (!running_ || session_timeout_ == 0) return;

    const auto now = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(session_timeout_);
    
    std::vector<std::string> sessions_to_close;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [session_id, dispatcher] : session_dispatchers_) {
            if (now - dispatcher->last_activity() > timeout) {
                // Exceeded idle time limit
                sessions_to_close.push_back(session_id);
            }
        }
    }
    
    // Close inactive sessions
    for (const auto& session_id : sessions_to_close) {
        LOG_INFO("Closing inactive session: ", session_id);
        
        close_session(session_id);
    }
}

bool server::set_mount_point(const std::string& mount_point, const std::string& dir, httplib::Headers headers) {
    return http_server_->set_mount_point(mount_point, dir, headers);
}

void server::close_session(const std::string& session_id) {
    // Snapshot state under lock. Idempotent: second concurrent caller finds
    // nothing to clean up and returns silently. Thread ownership stays in
    // sse_threads_ so server::stop() can join on shutdown.
    std::shared_ptr<event_dispatcher> dispatcher_to_close;
    std::map<std::string, session_cleanup_handler> cleanup_handlers_copy;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto dispatcher_it = session_dispatchers_.find(session_id);
        if (dispatcher_it == session_dispatchers_.end()) {
            // Already cleaned up by another caller — nothing to do.
            return;
        }

        dispatcher_to_close = dispatcher_it->second;
        session_dispatchers_.erase(dispatcher_it);

        session_initialized_.erase(session_id);
        session_protocol_versions_.erase(session_id);
        session_clients_.erase(session_id);
        session_log_levels_.erase(session_id);
        cancelled_requests_.erase(session_id);
        // Copy cleanup handlers so we can invoke them without holding the lock.
        cleanup_handlers_copy = session_cleanup_handler_;
    }

    // Close dispatcher outside the lock so threads waiting in wait_event
    // can wake immediately without contending for mutex_.
    if (dispatcher_to_close && !dispatcher_to_close->is_closed()) {
        dispatcher_to_close->close();
    }

    // Invoke cleanup handlers outside the lock. Handlers may do arbitrary
    // work including callbacks that re-enter the server; holding mutex_
    // would deadlock.
    try {
        for (const auto& [key, handler] : cleanup_handlers_copy) {
            try {
                handler(key);
            } catch (const std::exception& e) {
                LOG_WARNING("Session cleanup handler threw: ", session_id, ", ", e.what());
            } catch (...) {
                LOG_WARNING("Session cleanup handler threw unknown exception: ", session_id);
            }
        }
    } catch (...) {
        // Defensive — the inner try/catch should have caught everything.
    }

    // NOTE: we intentionally do NOT touch sse_threads_ here. The heartbeat
    // thread may itself be calling close_session during its own exit, so we
    // can't join from within it. server::stop() drains and joins all
    // entries in sse_threads_ on shutdown.
}

} // namespace mcp
