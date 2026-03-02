#include <gateway_app/gateway_app.hpp>

#include <iostream>
#include <string>

using namespace vix::web_app;
using namespace vix::api_app;
using namespace vix::gateway_app;

class FakeTransport : public ProxyTransport
{
public:
  Response forward(const Upstream &up, const Request &req) override
  {
    Response r;
    r.status = 200;
    r.body = "ok upstream=" + up.name + " path=" + req.path;
    return r;
  }
};

class RateLimitedGateway : public GatewayApplication
{
protected:
  void serve_once() override
  {
    // Simulate multiple requests with the same key
    for (int i = 0; i < 5; ++i)
    {
      Request req;
      req.method = HttpMethod::Get;
      req.path = "/users";

      req.headers["x-api-key"] = "demo-key";

      auto res = handle_gateway(req);
      std::cout << "request " << i << " -> " << res.status << " " << res.body << "\n";
    }

    stop();
  }
};

static std::string rate_key_from_request(const Request &req)
{
  auto it = req.headers.find("x-api-key");
  if (it != req.headers.end())
    return it->second;

  return "anonymous";
}

int main()
{
  RateLimitedGateway app;
  FakeTransport transport;

  GatewayConfig cfg;
  cfg.upstreams = {
      {"users-service", "http://users.internal"}};

  cfg.routes = {
      {"/users", "users-service"}};

  app.set_config(cfg);
  app.set_transport(&transport);

  // Allow 3 requests per 1 second per key
  FixedWindowRateLimiter limiter(3, std::chrono::milliseconds(1000));

  app.set_rate_limit_hook([&](const Request &req)
                          {
    const std::string key = rate_key_from_request(req);

    if (!limiter.allow(key))
      return PolicyDecision::deny(ApiApplication::unprocessable("rate limited", "rate_limited"));

    return PolicyDecision::allow(); });

  app.run();
}
