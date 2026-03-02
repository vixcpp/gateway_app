#include <gateway_app/gateway_app.hpp>

#include <iostream>

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
    r.headers["content-type"] = "text/plain";
    r.body = "proxy -> upstream=" + up.name + " base=" + up.base_url + " path=" + req.path;
    return r;
  }
};

class DemoGateway : public GatewayApplication
{
protected:
  void serve_once() override
  {
    // Simulate requests
    {
      Request req;
      req.method = HttpMethod::Get;
      req.path = "/users/list";
      auto res = handle_gateway(req);
      std::cout << "[GET /users/list] " << res.status << " " << res.body << "\n";
    }

    {
      Request req;
      req.method = HttpMethod::Get;
      req.path = "/orders/42";
      auto res = handle_gateway(req);
      std::cout << "[GET /orders/42] " << res.status << " " << res.body << "\n";
    }

    {
      Request req;
      req.method = HttpMethod::Get;
      req.path = "/missing";
      auto res = handle_gateway(req);
      std::cout << "[GET /missing] " << res.status << " " << res.body << "\n";
    }

    stop();
  }
};

int main()
{
  DemoGateway app;
  FakeTransport transport;

  GatewayConfig cfg;
  cfg.upstreams = {
      {"users-service", "http://users.internal"},
      {"orders-service", "http://orders.internal"}};

  cfg.routes = {
      {"/users", "users-service"},
      {"/orders", "orders-service"}};

  app.set_config(cfg);
  app.set_transport(&transport);

  // Optional request transform
  app.add_request_transform([](Request &req)
                            { req.headers["x-gateway"] = "vix"; });

  // Optional response transform
  app.add_response_transform([](Response &res)
                             { res.headers["x-powered-by"] = "vix/gateway_app"; });

  app.run();
}
