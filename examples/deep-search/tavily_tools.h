// examples/deep-search/tavily_tools.h
#ifndef EXAMPLES_DEEP_SEARCH_TAVILY_TOOLS_H_
#define EXAMPLES_DEEP_SEARCH_TAVILY_TOOLS_H_

#include <memory>
#include <string>

#include "agentflow/tools/tool.h"

namespace agentflow {
namespace net {
class IHttpClient;
}  // namespace net
}  // namespace agentflow

namespace deep_search {

// "tavily_search": POST https://api.tavily.com/search with {"query": ...,
// "max_results": 5, "search_depth": "basic"}. Returns a JSON string of
// [{title,url,content}] (trimmed) or {"error": "<message>"}.
std::shared_ptr<agentflow::Tool> MakeTavilySearchTool(
    agentflow::net::IHttpClient& http, std::string api_key);

// "tavily_extract": POST https://api.tavily.com/extract with {"urls": [...],
// "extract_depth": "basic", "format": "markdown"} (max 20 URLs; Tavily
// fetches them server-side). Returns a JSON string of
// [{url,raw_content}] plus failed_results, or {"error": "<message>"}.
std::shared_ptr<agentflow::Tool> MakeTavilyExtractTool(
    agentflow::net::IHttpClient& http, std::string api_key);

}  // namespace deep_search

#endif  // EXAMPLES_DEEP_SEARCH_TAVILY_TOOLS_H_
