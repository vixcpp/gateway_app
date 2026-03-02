# gateway_app

API gateway kit for modern C++.

`gateway_app` provides a minimal, deterministic gateway layer built on top of `vix/api_app`.

It enables:

- Upstream registry
- Prefix-based routing
- Auth policies
- Rate limiting policies
- Request/response transforms
- Transport abstraction

Header-only. Layered. Explicit.

## Download

https://vixcpp.com/registry/pkg/vix/gateway_app

## Why gateway_app?

Every distributed system eventually needs:

- Centralized routing
- Edge authentication
- Rate limiting
- Service aggregation
- Reverse proxy logic
- Policy enforcement

Most C++ backends either:

- Hardcode upstream calls
- Mix proxy logic with business code
- Reimplement auth checks everywhere
- Couple transport and policy logic tightly

`gateway_app` provides:

- Deterministic routing
- Pluggable policies
- Clean transport abstraction
- Clear layering over API runtime

No forced networking implementation.

No HTTP client included.
No service mesh required.
No distributed rate limiter built-in.

Just a structured gateway foundation.

## Dependency

`gateway_app` depends on:

- `vix/api_app`
- (transitively) `vix/web_app`
- (transitively) `vix/app`

Architecture layering:

```
vix/app
  ↑
vix/web_app
  ↑
vix/api_app
  ↑
vix/gateway_app
```

This ensures:

- Stable foundation
- Strict module boundaries
- No circular dependencies
- Composable infrastructure layers

Dependencies are installed automatically via Vix Registry.

## Installation

### Using Vix Registry

```bash
vix add vix/gateway_app
vix deps
```

### Manual

```bash
git clone https://github.com/vixcpp/gateway_app.git
```

Add the `include/` directory and ensure dependencies are available.

## Core concepts

### Upstreams

```cpp
GatewayConfig cfg;
cfg.upstreams = {
    {"users-service", "http://users.internal"},
    {"orders-service", "http://orders.internal"}
};
```

### Routes (prefix matching)

```cpp
cfg.routes = {
    {"/users", "users-service"},
    {"/orders", "orders-service"}
};
```

Routing uses **longest-prefix match**.

### Transport abstraction

`gateway_app` does not implement network I/O.

You must provide a `ProxyTransport`:

```cpp
class MyTransport : public ProxyTransport
{
public:
    Response forward(const Upstream& up, const Request& req) override
    {
        // Call real HTTP client here
        Response r;
        r.status = 200;
        r.body = "forwarded to " + up.name;
        return r;
    }
};
```

This design keeps:

- Networking separate
- Gateway deterministic
- Transport swappable

## Policy hooks

### Auth hook

```cpp
app.set_auth_hook([](const Request& req) {
    if (req.headers.find("authorization") == req.headers.end())
        return PolicyDecision::deny(
            ApiApplication::unauthorized("missing token")
        );

    return PolicyDecision::allow();
});
```

### Rate limit hook

```cpp
FixedWindowRateLimiter limiter(10, std::chrono::milliseconds(1000));

app.set_rate_limit_hook([&](const Request&) {
    if (!limiter.allow("global"))
        return PolicyDecision::deny(
            ApiApplication::unprocessable("rate limited", "rate_limited")
        );

    return PolicyDecision::allow();
});
```

## Request/response transforms

Modify request before forwarding:

```cpp
app.add_request_transform([](Request& req) {
    req.headers["x-gateway"] = "vix";
});
```

Modify response after forwarding:

```cpp
app.add_response_transform([](Response& res) {
    res.headers["x-powered-by"] = "vix/gateway_app";
});
```

## Main gateway flow

`handle_gateway()` performs:

1. Route resolution
2. Upstream lookup
3. Auth policy
4. Rate limit policy
5. Request transforms
6. Proxy forward
7. Response transforms
8. Return response

All steps are explicit and deterministic.

## Complexity

| Operation          | Complexity |
|-------------------|------------|
| Route resolution   | O(n) prefix scan |
| Upstream lookup    | O(1) average |
| Policy hooks       | O(1) |
| Forwarding         | depends on transport |
| Rate limiter (local) | O(1) average |

For small-to-medium route sets, linear prefix scan is predictable and stable.

## Design philosophy

`gateway_app` focuses on:

- Explicit infrastructure layers
- Deterministic policy flow
- No hidden network threads
- Clear separation of transport and policy
- Minimal surface area
- Infrastructure composability

It does not aim to replace:

- Full service mesh
- Distributed rate limit systems
- Production reverse proxies
- High-performance HTTP clients

Those belong to transport or higher-level infrastructure layers.

## Tests

Run:

```bash
vix build
vix test
```

Tests verify:

- Route resolution
- Upstream mapping
- Policy enforcement
- Transport forwarding
- Error handling

## License

MIT License\
Copyright (c) Gaspard Kirira

