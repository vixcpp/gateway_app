/**
 * @file gateway_app.hpp
 * @brief API gateway kit built on top of vix/api_app for routing and edge policies.
 *
 * `gateway_app` provides a minimal, deterministic gateway foundation:
 *
 * - Upstream registry (named upstream targets)
 * - Prefix-based routing rules (path -> upstream)
 * - Policy hooks (auth, rate limit, request/response transforms)
 * - Proxy abstraction (no mandatory HTTP client dependency)
 *
 * This module is intentionally framework-agnostic.
 * It does not implement a real reverse proxy transport.
 * Instead, it defines a small `ProxyTransport` interface that you can bind to:
 *
 * - a real HTTP client (future vix net/http client)
 * - an embedded service mesh
 * - a custom edge runtime
 *
 * Requirements: C++17+
 * Header-only. Depends on `vix/api_app`.
 */

#ifndef VIX_GATEWAY_APP_GATEWAY_APP_HPP
#define VIX_GATEWAY_APP_GATEWAY_APP_HPP

#include <api_app/api_app.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vix::gateway_app
{

  /**
   * @brief Upstream endpoint descriptor.
   *
   * `base_url` is an opaque string for now.
   * A real transport binding may interpret it as:
   * - http://host:port
   * - https://service.internal
   * - unix://path
   */
  struct Upstream
  {
    std::string name;
    std::string base_url;
  };

  /**
   * @brief Route rule: map path prefix to an upstream.
   *
   * Example:
   * - prefix "/users" -> upstream "users-service"
   */
  struct RouteRule
  {
    std::string prefix;
    std::string upstream;
  };

  /**
   * @brief Gateway configuration.
   *
   * This is a minimal configuration object designed to be filled
   * programmatically or loaded by a higher-level layer.
   */
  struct GatewayConfig
  {
    std::vector<Upstream> upstreams;
    std::vector<RouteRule> routes;
  };

  /**
   * @brief Policy decision result.
   *
   * If `allowed == false`, `response` must be set and is returned immediately.
   */
  struct PolicyDecision
  {
    bool allowed = true;
    std::optional<vix::web_app::Response> response;

    static PolicyDecision allow()
    {
      return PolicyDecision{true, std::nullopt};
    }

    static PolicyDecision deny(vix::web_app::Response r)
    {
      PolicyDecision d;
      d.allowed = false;
      d.response = std::move(r);
      return d;
    }
  };

  /**
   * @brief Proxy transport interface for forwarding requests to upstreams.
   *
   * Gateway does not provide a network implementation. A binding should implement:
   * - translate Request -> upstream request
   * - perform IO
   * - translate upstream response -> Response
   */
  class ProxyTransport
  {
  public:
    virtual ~ProxyTransport() = default;

    /**
     * @brief Forward a request to the given upstream.
     * @param upstream Upstream endpoint descriptor
     * @param req Incoming gateway request
     * @return Response returned by upstream
     */
    virtual vix::web_app::Response forward(const Upstream &upstream,
                                           const vix::web_app::Request &req) = 0;
  };

  /**
   * @brief Simple fixed-window rate limiter (in-memory).
   *
   * This limiter is deterministic and minimal.
   * It is intended as a default policy implementation, not a production distributed limiter.
   *
   * Key:
   * - Typically user key, IP, token id, or "global".
   *
   * Parameters:
   * - max_requests: allowed requests per window
   * - window: duration of the window
   */
  class FixedWindowRateLimiter
  {
  public:
    FixedWindowRateLimiter() = default;

    FixedWindowRateLimiter(std::uint32_t max_requests,
                           std::chrono::milliseconds window)
        : max_requests_(max_requests),
          window_(window)
    {
      if (max_requests_ == 0)
        throw std::runtime_error("FixedWindowRateLimiter: max_requests must be > 0");
      if (window_.count() <= 0)
        throw std::runtime_error("FixedWindowRateLimiter: window must be > 0ms");
    }

    /**
     * @brief Check and consume one request for a given key.
     * @return true if allowed, false if rate-limited
     */
    bool allow(std::string_view key)
    {
      const auto now = Clock::now();
      auto &s = states_[std::string(key)];

      if (!s.initialized)
      {
        s.initialized = true;
        s.window_start = now;
        s.count = 0;
      }

      if (now - s.window_start >= window_)
      {
        s.window_start = now;
        s.count = 0;
      }

      if (s.count >= max_requests_)
        return false;

      ++s.count;
      return true;
    }

    std::uint32_t max_requests() const noexcept { return max_requests_; }
    std::chrono::milliseconds window() const noexcept { return window_; }

  private:
    using Clock = std::chrono::steady_clock;

    struct State
    {
      bool initialized = false;
      Clock::time_point window_start{};
      std::uint32_t count = 0;
    };

    std::uint32_t max_requests_ = 0;
    std::chrono::milliseconds window_{0};
    std::unordered_map<std::string, State> states_;
  };

  /**
   * @brief API gateway application built on top of vix/api_app.
   *
   * Core responsibilities:
   * - Find route -> upstream
   * - Apply policies (auth, rate limit, transforms)
   * - Forward to upstream via ProxyTransport
   * - Return upstream response
   */
  class GatewayApplication : public vix::api_app::ApiApplication
  {
  public:
    /// Auth hook signature. Return deny() to block, allow() to continue.
    using AuthHook = std::function<PolicyDecision(const vix::web_app::Request &)>;

    /// Rate limit hook signature. Return deny() to block, allow() to continue.
    using RateLimitHook = std::function<PolicyDecision(const vix::web_app::Request &)>;

    /// Request transform hook (in-place).
    using RequestTransform = std::function<void(vix::web_app::Request &)>;

    /// Response transform hook (in-place).
    using ResponseTransform = std::function<void(vix::web_app::Response &)>;

    GatewayApplication() = default;
    ~GatewayApplication() override = default;

    GatewayApplication(const GatewayApplication &) = delete;
    GatewayApplication &operator=(const GatewayApplication &) = delete;

    GatewayApplication(GatewayApplication &&) = delete;
    GatewayApplication &operator=(GatewayApplication &&) = delete;

    /**
     * @brief Load gateway configuration (upstreams and routes).
     *
     * Existing configuration is replaced.
     */
    void set_config(GatewayConfig cfg)
    {
      cfg_ = std::move(cfg);
      rebuild_indexes();
    }

    /**
     * @brief Set proxy transport used for forwarding requests.
     *
     * Must be set before calling handle_gateway().
     */
    void set_transport(ProxyTransport *transport) noexcept
    {
      transport_ = transport;
    }

    /**
     * @brief Set optional auth policy hook.
     */
    void set_auth_hook(AuthHook hook)
    {
      auth_hook_ = std::move(hook);
    }

    /**
     * @brief Set optional rate limit policy hook.
     */
    void set_rate_limit_hook(RateLimitHook hook)
    {
      rate_hook_ = std::move(hook);
    }

    /**
     * @brief Add a request transform hook.
     *
     * Transforms run after policies and before forwarding.
     */
    void add_request_transform(RequestTransform t)
    {
      req_transforms_.push_back(std::move(t));
    }

    /**
     * @brief Add a response transform hook.
     *
     * Transforms run after forwarding and before returning to caller.
     */
    void add_response_transform(ResponseTransform t)
    {
      res_transforms_.push_back(std::move(t));
    }

    /**
     * @brief Main gateway handler.
     *
     * It performs:
     * - route resolution
     * - auth policy
     * - rate limit policy
     * - request transforms
     * - forward via ProxyTransport
     * - response transforms
     *
     * @return The final response to return to the caller.
     */
    vix::web_app::Response handle_gateway(vix::web_app::Request req) const
    {
      if (!transport_)
        return vix::api_app::ApiApplication::internal_error("gateway transport not set", "transport_not_set");

      const auto route = resolve_route(req.path);
      if (!route)
        return vix::api_app::ApiApplication::not_found("no route for path", "route_not_found");

      const Upstream *up = find_upstream(route->upstream);
      if (!up)
        return vix::api_app::ApiApplication::internal_error("upstream not found", "upstream_not_found");

      if (auth_hook_)
      {
        PolicyDecision d = auth_hook_(req);
        if (!d.allowed && d.response)
          return *d.response;
        if (!d.allowed)
          return vix::api_app::ApiApplication::forbidden("blocked by auth policy", "auth_denied");
      }

      if (rate_hook_)
      {
        PolicyDecision d = rate_hook_(req);
        if (!d.allowed && d.response)
          return *d.response;
        if (!d.allowed)
          return vix::api_app::ApiApplication::unprocessable("blocked by rate policy", "rate_limited");
      }

      for (const auto &t : req_transforms_)
        t(req);

      vix::web_app::Response res = transport_->forward(*up, req);

      for (const auto &t : res_transforms_)
        t(res);

      return res;
    }

    /**
     * @brief Install a default route into the underlying router to use the gateway handler.
     *
     * This registers:
     * - GET  <prefix>
     * - POST <prefix>
     * - PUT  <prefix>
     * - PATCH<prefix>
     * - DELETE<prefix>
     *
     * Notes:
     * - The router in `web_app` is exact-match, so this helper registers each configured prefix
     *   as an exact match.
     * - For real prefix routing, a higher-level router is recommended.
     */
    void install_routes_as_exact_matches()
    {
      // Because web_app router matches exact paths, we register configured prefixes as exact routes.
      // This is still useful for simple gateways and testing.
      for (const auto &r : cfg_.routes)
      {
        router().get(r.prefix, [this](const vix::web_app::Request &req)
                     { return this->handle_gateway(req); });

        router().post(r.prefix, [this](const vix::web_app::Request &req)
                      { return this->handle_gateway(req); });

        router().add(vix::web_app::HttpMethod::Put, r.prefix, [this](const vix::web_app::Request &req)
                     { return this->handle_gateway(req); });

        router().add(vix::web_app::HttpMethod::Patch, r.prefix, [this](const vix::web_app::Request &req)
                     { return this->handle_gateway(req); });

        router().add(vix::web_app::HttpMethod::Delete, r.prefix, [this](const vix::web_app::Request &req)
                     { return this->handle_gateway(req); });
      }
    }

  private:
    std::optional<RouteRule> resolve_route(std::string_view path) const
    {
      // Longest-prefix match (simple deterministic scan).
      // We keep it simple and stable for 0.1.0.
      const RouteRule *best = nullptr;

      for (const auto &r : cfg_.routes)
      {
        if (r.prefix.empty())
          continue;

        if (path.size() < r.prefix.size())
          continue;

        if (path.compare(0, r.prefix.size(), r.prefix) == 0)
        {
          if (!best || r.prefix.size() > best->prefix.size())
            best = &r;
        }
      }

      if (!best)
        return std::nullopt;

      return *best;
    }

    const Upstream *find_upstream(std::string_view name) const
    {
      const auto it = upstream_index_.find(std::string(name));
      if (it == upstream_index_.end())
        return nullptr;
      return it->second;
    }

    void rebuild_indexes()
    {
      upstream_index_.clear();
      upstream_index_.reserve(cfg_.upstreams.size());

      for (auto &u : cfg_.upstreams)
        upstream_index_[u.name] = &u;
    }

  private:
    GatewayConfig cfg_{};
    ProxyTransport *transport_ = nullptr;

    AuthHook auth_hook_{};
    RateLimitHook rate_hook_{};

    std::vector<RequestTransform> req_transforms_{};
    std::vector<ResponseTransform> res_transforms_{};

    std::unordered_map<std::string, Upstream *> upstream_index_{};
  };

} // namespace vix::gateway_app

#endif // VIX_GATEWAY_APP_GATEWAY_APP_HPP
