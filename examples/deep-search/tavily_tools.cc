// examples/deep-search/tavily_tools.cc
#include "examples/deep-search/tavily_tools.h"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include <asio/awaitable.hpp>

#include "agentflow/net/http_client.h"
#include "agentflow/tools/native_fn_tool.h"

namespace deep_search {
namespace {

using json = nlohmann::json;

constexpr char kTavilyBase[] = "https://api.tavily.com";

agentflow::net::HttpRequest MakeRequest(std::string path, std::string body,
                                        std::string_view api_key) {
  agentflow::net::HttpRequest req;
  req.url = std::string(kTavilyBase) + path;
  req.body = std::move(body);
  req.headers.push_back({"Content-Type", "application/json"});
  req.headers.push_back(
      {"Authorization", std::string("Bearer ") + std::string(api_key)});
  return req;
}

}  // namespace

std::shared_ptr<agentflow::Tool> MakeTavilySearchTool(
    agentflow::net::IHttpClient& http, std::string api_key) {
  agentflow::ToolSchema schema{
      .name = "tavily_search",
      .description =
          "Search the web via Tavily. Returns title, url and content for "
          "each result.",
      .params_json_schema =
          R"({"type":"object","properties":{"query":{"type":"string"}},)"
          R"("required":["query"]})"};
  auto fn = [&http, api_key = std::move(api_key)](
                std::string_view args_json,
                std::string_view,
                const agentflow::CancelToken& cancel)
                -> asio::awaitable<std::string> {
    json args = json::parse(args_json, nullptr, /*allow_exceptions=*/false);
    std::string query =
        (args.is_object() && args.contains("query") &&
         args["query"].is_string())
            ? args["query"].get<std::string>()
            : std::string{};
    if (query.empty()) co_return std::string(R"({"error":"bad_args"})");
    json body = {{"query", query}, {"max_results", 5}, {"search_depth", "basic"}};
    auto resp = co_await http.Post(MakeRequest("/search", body.dump(), api_key),
                                   cancel);
    if (!resp.ok()) co_return std::string(R"({"error":"http_error"})");
    json parsed = json::parse(*resp, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() ||
        !parsed.contains("results") || !parsed["results"].is_array()) {
      co_return std::string(R"({"error":"bad_response"})");
    }
    json out = json::array();
    for (const auto& r : parsed["results"]) {
      if (!r.is_object()) continue;
      json item;
      if (r.contains("title") && r["title"].is_string())
        item["title"] = r["title"];
      if (r.contains("url") && r["url"].is_string()) item["url"] = r["url"];
      if (r.contains("content") && r["content"].is_string())
        item["content"] = r["content"];
      out.push_back(std::move(item));
    }
    co_return out.dump();
  };
  return std::make_shared<agentflow::NativeFnTool>(std::move(schema),
                                                    std::move(fn));
}

std::shared_ptr<agentflow::Tool> MakeTavilyExtractTool(
    agentflow::net::IHttpClient& http, std::string api_key) {
  agentflow::ToolSchema schema{
      .name = "tavily_extract",
      .description =
          "Read web pages via Tavily. Provide up to 20 URLs in one call; "
          "they are fetched server-side. Returns {url, raw_content} "
          "entries.",
      .params_json_schema =
          R"({"type":"object","properties":{"urls":{"type":"array",)"
          R"("items":{"type":"string"}}},"required":["urls"]})"};
  auto fn = [&http, api_key = std::move(api_key)](
                std::string_view args_json,
                std::string_view,
                const agentflow::CancelToken& cancel)
                -> asio::awaitable<std::string> {
    json args = json::parse(args_json, nullptr, /*allow_exceptions=*/false);
    if (!args.is_object() || !args.contains("urls") ||
        !args["urls"].is_array()) {
      co_return std::string(R"({"error":"bad_args"})");
    }
    json urls = json::array();
    for (const auto& u : args["urls"]) {
      if (u.is_string()) urls.push_back(u.get<std::string>());
      if (urls.size() >= 20) break;  // Tavily hard limit
    }
    if (urls.empty()) co_return std::string(R"({"error":"bad_args"})");
    json body = {{"urls", urls},
                 {"extract_depth", "basic"},
                 {"format", "markdown"}};
    auto resp = co_await http.Post(
        MakeRequest("/extract", body.dump(), api_key), cancel);
    if (!resp.ok()) co_return std::string(R"({"error":"http_error"})");
    json parsed = json::parse(*resp, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() ||
        !parsed.contains("results") || !parsed["results"].is_array()) {
      co_return std::string(R"({"error":"bad_response"})");
    }
    json out;
    out["results"] = json::array();
    for (const auto& r : parsed["results"]) {
      if (!r.is_object()) continue;
      json item;
      if (r.contains("url") && r["url"].is_string()) item["url"] = r["url"];
      if (r.contains("raw_content") && r["raw_content"].is_string()) {
        std::string raw = r["raw_content"].get<std::string>();
        constexpr size_t kMaxLen = 6000;  // trim huge pages for the LLM
        if (raw.size() > kMaxLen) raw.resize(kMaxLen);
        item["raw_content"] = std::move(raw);
      }
      out["results"].push_back(std::move(item));
    }
    if (parsed.contains("failed_results") &&
        parsed["failed_results"].is_array()) {
      out["failed_results"] = parsed["failed_results"];
    }
    co_return out.dump();
  };
  return std::make_shared<agentflow::NativeFnTool>(std::move(schema),
                                                    std::move(fn));
}

}  // namespace deep_search
