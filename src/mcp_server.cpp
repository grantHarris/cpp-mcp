/**
 * @file mcp_server.cpp
 * @brief Implementation of the MCP server
 * 
 * This file implements the server-side functionality for the Model Context Protocol.
 * Follows the 2024-11-05 basic protocol specification.
 */

#include "mcp_server.h"
#include <sys/stat.h>

namespace {
bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
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
    , thread_pool_(conf.threadpool_size)
    , max_sessions_(conf.max_sessions)
    , session_timeout_(conf.session_timeout)
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
    http_server_->Options(".*", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Accept, Mcp-Session-Id");
        res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");
        res.status = 204; // No Content
    });

    // Setup JSON-RPC endpoint (SSE transport)
    http_server_->Post(msg_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_jsonrpc(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" ", res.status);
    });

    // Setup SSE endpoint (legacy 2024-11-05 transport)
    http_server_->Get(sse_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_sse(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" ", res.status);
    });

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
    
    // Start resource check thread (only start in non-blocking mode)
    if (!blocking) {
        maintenance_thread_run_ = true;
        maintenance_thread_ = std::make_unique<std::thread>([this]() {
            while (true) {
                // Check inactive sessions every 10 seconds
                std::unique_lock<std::mutex> lock(maintenance_mutex_);
                auto should_exit = maintenance_cond_.wait_for(lock, std::chrono::seconds(10), [this] {
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
    
    // Start server
    if (blocking) {
        running_ = true;
        LOG_INFO("Starting server in blocking mode");
        if (!http_server_->listen(host_.c_str(), port_)) {
            running_ = false;
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

    // Close maintenance thread
    if (maintenance_thread_ && maintenance_thread_->joinable()) {
        {
            std::unique_lock<std::mutex> lock(maintenance_mutex_);
            maintenance_thread_run_ = false;
        }

        maintenance_cond_.notify_one();

        try {
            maintenance_thread_->join();
        } catch (...) {
            maintenance_thread_->detach();
        }
    }
    
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
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
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
            size_t page_size = 100;
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
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
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
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
            }
            return json::object();
        };
    }

    if (method_handlers_.find("resources/templates/list") == method_handlers_.end()) {
        method_handlers_["resources/templates/list"] = [this](const json& params, const std::string& session_id) -> json {
            size_t start = 0;
            size_t page_size = 100;
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
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
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
            size_t page_size = 100;
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
            size_t page_size = 100;
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
            if (!params.contains("name")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'name' parameter");
            }
            
            std::string tool_name = params["name"];
            auto it = tools_.find(tool_name);
            if (it == tools_.end()) {
                throw mcp_exception(error_code::invalid_params, "Tool not found: " + tool_name);
            }
            
            json tool_args = params.contains("arguments") ? params["arguments"] : json::array();

            if (tool_args.is_string()) {
                try {
                    tool_args = json::parse(tool_args.get<std::string>());
                } catch (const json::exception& e) {
                    throw mcp_exception(error_code::invalid_params, "Invalid JSON arguments: " + std::string(e.what()));
                }
            }

            json tool_result = {
                {"isError", false}
            };

            try {
                tool_result["content"] = it->second.second(tool_args, session_id);
            } catch (const std::exception& e) {
                tool_result["isError"] = true;
                tool_result["content"] = json::array({
                    {
                        {"type", "text"},
                        {"text", e.what()}
                    }
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
            size_t page_size = 100;
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
            if (!params.contains("name")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'name' parameter");
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
    auth_handler_ = handler;
}

void server::handle_sse(const httplib::Request& req, httplib::Response& res) {
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
    res.set_header("Access-Control-Allow-Origin", "*");
    
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
            
            // Send periodic heartbeats to detect connection status. Use an
            // interruptible wait so server::stop() (which calls dispatcher
            // close()) can wake this thread immediately — previously a naked
            // sleep_for(5s) meant stop() had to either wait up to 5s or
            // detach the thread (hazardous: detached thread outlives server
            // and crashes on use-after-free).
            int heartbeat_count = 0;
            while (running_ && !session_dispatcher->is_closed()) {
                auto timeout = std::chrono::seconds(5) +
                               std::chrono::milliseconds(rand() % 500);
                if (session_dispatcher->wait_for_close(timeout)) {
                    break; // Dispatcher closed — exit cleanly
                }

                if (session_dispatcher->is_closed() || !running_) {
                    break;
                }
                
                std::stringstream heartbeat;
                heartbeat << "event: heartbeat\r\ndata: " << heartbeat_count++ << "\r\n\r\n";
                
                try {
                    bool sent = session_dispatcher->send_event(heartbeat.str());
                    if (!sent) {
                        LOG_WARNING("Failed to send heartbeat, client may have closed connection: ", session_id);
                        break;
                    }
                    
                    // Update activity time (heartbeat successful)
                    session_dispatcher->update_activity();
                } catch (const std::exception& e) {
                    LOG_ERROR("Failed to send heartbeat: ", e.what());
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
    // Setup response headers
    res.set_header("Content-Type", "application/json");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    
    // Handle OPTIONS request (CORS pre-flight)
    if (req.method == "OPTIONS") {
        res.status = 204; // No Content
        return;
    }
    
    // Get session ID
    auto it = req.params.find("session_id");
    std::string session_id = it != req.params.end() ? it->second : "";

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
        thread_pool_.enqueue([this, mcp_req, session_id]() {
            process_request(mcp_req, session_id);
        });
        
        // Return 202 Accepted
        res.status = 202;
        res.set_content("Accepted", "text/plain");
        return;
    }
    
    // For requests with ID, process it asynchronously in the thread pool and return the result via SSE
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
    
    // Return 202 Accepted
    res.status = 202;
    res.set_content("Accepted", "text/plain");
}

// ---------------------------------------------------------------------------
// Streamable HTTP transport (2025-03-26 spec)
// ---------------------------------------------------------------------------

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
    request req;
    req.jsonrpc = j.value("jsonrpc", "2.0");
    if (j.contains("id") && !j["id"].is_null()) {
        req.id = j["id"];
    }
    if (j.contains("method")) {
        req.method = j["method"].get<std::string>();
    }
    if (j.contains("params")) {
        req.params = j["params"];
    }
    return req;
}

void server::handle_mcp_post(const httplib::Request& req, httplib::Response& res) {
    // CORS headers
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers",
                   "Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version");
    res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id, MCP-Protocol-Version");

    // Reflect the protocol version of this exchange on every response.
    {
        std::string sid = req.get_header_value("Mcp-Session-Id");
        std::string ver = !sid.empty() ? session_protocol_version(sid) : "";
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

    // Get or create session
    std::string session_id = req.get_header_value("Mcp-Session-Id");

    // Check if this is an initialize request (no session needed)
    bool is_initialize = false;
    if (body.is_object() && body.contains("method") && body["method"] == "initialize") {
        is_initialize = true;
    }

    // Spec 2025-06-18 removed JSON-RPC batching; reject array bodies outright.
    if (body.is_array()) {
        res.status = 400;
        res.set_content(
            response::create_error(nullptr, error_code::invalid_request,
                                   "JSON-RPC batching is not supported (spec 2025-06-18+)")
                .to_json().dump(),
            "application/json");
        return;
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_dispatchers_.find(session_id) == session_dispatchers_.end()) {
                // Session expired or invalid — client must re-initialize
                res.status = 404;
                res.set_content("{\"error\":\"Session not found\"}", "application/json");
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
    }

    // Notifications and responses (no id, or id=null): fire and forget.
    bool has_request_id = body.contains("id") && !body["id"].is_null();
    if (!has_request_id) {
        if (!session_id.empty()) {
            auto mcp_req = parse_jsonrpc_message(body);
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

        auto mcp_req = parse_jsonrpc_message(body);
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
    auto mcp_req = parse_jsonrpc_message(body);
    json result = process_request(mcp_req, session_id);
    res.set_header("Content-Type", "application/json");
    res.set_content(result.dump(), "application/json");
}

void server::handle_mcp_get(const httplib::Request& req, httplib::Response& res) {
    // CORS headers
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Headers",
                   "Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version");
    res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id, MCP-Protocol-Version");

    std::string session_id = req.get_header_value("Mcp-Session-Id");
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
    res.set_header("Access-Control-Allow-Origin", "*");

    std::string session_id = req.get_header_value("Mcp-Session-Id");
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
            handler(req.params, session_id);
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
            if (!req.params.contains("level")) {
                return response::create_error(req.id, error_code::invalid_params,
                    "Missing 'level' parameter").to_json();
            }
            std::string level = req.params["level"].get<std::string>();
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
            e.what()
        ).to_json();
    } catch (const std::exception& e) {
        // Other exceptions
        LOG_ERROR("Exception while processing request: ", e.what());
        return response::create_error(
            req.id,
            error_code::internal_error,
            "Internal error: " + std::string(e.what())
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
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    ss << std::hex;
    
    // UUID format: 8-4-4-4-12 hexadecimal digits
    for (int i = 0; i < 8; ++i) {
        ss << dis(gen);
    }
    ss << "-";
    
    for (int i = 0; i < 4; ++i) {
        ss << dis(gen);
    }
    ss << "-";
    
    for (int i = 0; i < 4; ++i) {
        ss << dis(gen);
    }
    ss << "-";
    
    for (int i = 0; i < 4; ++i) {
        ss << dis(gen);
    }
    ss << "-";
    
    for (int i = 0; i < 12; ++i) {
        ss << dis(gen);
    }
    
    return ss.str();
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
