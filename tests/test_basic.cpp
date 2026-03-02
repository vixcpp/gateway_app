#include <gateway_app/gateway_app.hpp>

#include <cassert>
#include <iostream>

using namespace vix::web_app;
using namespace vix::api_app;
using namespace vix::gateway_app;

/**
 * Fake transport used for testing.
 * It does not perform network IO.
 * It simply echoes upstream name and path.
 */
class FakeTransport : public ProxyTransport
{
public:
  Response forward(const Upstream &up,
                   const Request &req) override
  {
    Response r;
    r.status = 200;
    r.body = "forwarded to " + up.name + " path=" + req.path;
    return r;
  }
};

class TestGateway : public GatewayApplication
{
protected:
  void serve_once() override
  {
    // 1) Normal route forwarding
    {
      Request req;
      req.method = HttpMethod::Get;
      req.path = "/users/list";

      auto res = handle_gateway(req);

      assert(res.status == 200);
      assert(res.body.find("users-service") != std::string::npos);
    }

    // 2) Route not found
    {
      Request req;
      req.method = HttpMethod::Get;
      req.path = "/unknown";

      auto res = handle_gateway(req);

      assert(res.status == 404);
    }

    // 3) Auth denial
    {
      Request req;
      req.method = HttpMethod::Get;
      req.path = "/users/list";

      auth_block_ = true;
      auto res = handle_gateway(req);
      auth_block_ = false;

      assert(res.status == 403);
    }

    // 4) Rate limit denial
    {
      Request req;
      req.method = HttpMethod::Get;
      req.path = "/users/list";

      rate_block_ = true;
      auto res = handle_gateway(req);
      rate_block_ = false;

      assert(res.status == 422);
    }

    stop();
  }

public:
  bool auth_block_ = false;
  bool rate_block_ = false;
};

int main()
{
  TestGateway app;
  FakeTransport transport;

  // Configure upstreams
  GatewayConfig cfg;
  cfg.upstreams = {
      {"users-service", "http://users.internal"},
      {"orders-service", "http://orders.internal"}};

  cfg.routes = {
      {"/users", "users-service"},
      {"/orders", "orders-service"}};

  app.set_config(cfg);
  app.set_transport(&transport);

  // Install auth hook
  app.set_auth_hook([&](const Request &)
                    {
    if (app.auth_block_)
      return PolicyDecision::deny(
          ApiApplication::forbidden("blocked by test auth"));
    return PolicyDecision::allow(); });

  // Install rate hook
  app.set_rate_limit_hook([&](const Request &)
                          {
    if (app.rate_block_)
      return PolicyDecision::deny(
          ApiApplication::unprocessable("rate limited"));
    return PolicyDecision::allow(); });

  app.run();

  // Test: transport not set case
  {
    TestGateway app2;
    app2.set_config(cfg);

    Request req;
    req.method = HttpMethod::Get;
    req.path = "/users/list";

    auto res = app2.handle_gateway(req);
    assert(res.status == 500);
  }

  return 0;
}
