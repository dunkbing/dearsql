# acp-cpp

A C++20 client library for the [Agent Client Protocol](https://agentclientprotocol.com):
talk to coding agents (Claude Code, Gemini CLI, Codex, …) over JSON-RPC on their
stdio, the same way editors like Zed do.

It follows the shape of the official and community SDKs: you implement
`acp::Client` (what the agent calls on you) and drive an `acp::Connection`
(what you call on the agent).

Extracted from [DearSQL](http://github.com/dunkbing/dearsql).

```cpp
#include <acp/connection.hpp>

struct MyClient : acp::Client {
    acp::Connection* conn = nullptr;

    void sessionUpdate(const acp::SessionNotification& n) override {
        if (n.update.kind == acp::SessionUpdate::Kind::AgentMessageChunk)
            std::cout << n.update.text;
    }
    void requestPermission(const acp::PermissionRequest& req) override {
        conn->respondPermission(req.rpcId, req.options.front().optionId);
    }
};

MyClient client;
auto [conn, err] = acp::Connection::spawn(client, {"claude-agent-acp"}, {.cwd = "/work"});
client.conn = conn.get();

conn->initialize().get();
auto session = conn->newSession("/work").get();
auto id = session.result["sessionId"].get<std::string>();
conn->prompt(id, acp::json::array({acp::textBlock("list the tables")})).get();
```

Every request returns a `std::future<acp::Response>` and optionally takes a
callback that fires on the reader thread when the reply lands — use whichever
fits your event loop. Callbacks on `acp::Client` also run on the reader thread.

## What is in the box

| Header | Contents |
|--------|----------|
| `acp/connection.hpp` | `Connection`: spawn an agent, `initialize`, `authenticate`, `newSession`, `loadSession`, `prompt`, `cancel`, `setSessionMode`, raw `request`/`notify` for extensions |
| `acp/client.hpp` | `Client` interface: `sessionUpdate`, `requestPermission`, optional `readTextFile`/`writeTextFile`/`extRequest`, process lifecycle hooks |
| `acp/types.hpp` | `SessionUpdate`, `ToolCall`, `PlanEntry`, `PermissionRequest`, `InitializeResult`, MCP server descriptors, content-block helpers |
| `acp/agents.hpp` | built-in catalog (Claude Code, Gemini CLI, Codex) and launch resolution: binary on PATH → `npx`/`bunx`/`pnpm dlx`/`yarn dlx`/`uvx` → install command |
| `acp/registry.hpp` | the official agent registry: fetch it, download a prebuilt binary (SHA-256 verified) into `installRoot()` |
| `acp/log.hpp` | `setLogger` — silent by default |

## Building

```cmake
add_subdirectory(external/acp-cpp)
target_link_libraries(myapp PRIVATE acp::acp)
```

Dependencies: [nlohmann_json](https://github.com/nlohmann/json) and threads.
`-DACP_REGISTRY=ON` adds registry download support and needs
[cpp-httplib](https://github.com/yhirose/cpp-httplib) and OpenSSL.
`-DACP_BUILD_EXAMPLES=ON` builds `acp-chat`, a tiny terminal client:

```
acp-chat npx --yes @agentclientprotocol/claude-agent-acp
```

## Notes

- macOS and Linux. Windows compiles but `Connection::spawn` reports unsupported
  until someone writes the `CreateProcess` transport.
- The agent gets its own process group so `stop()` also reaches the `node`
  grandchild that `npx` leaves behind.
- `SpawnOptions::env` / `dropEnv` let you fix up the environment — GUI apps
  typically pass the login shell's `PATH` (`acp::agents::loginShellPath()`) and
  drop `CLAUDECODE`, which makes Claude Code refuse to start when it believes it
  is nested.
- No fs or terminal support is advertised unless you override those `Client`
  methods and set the matching `ClientCapabilities` in `initialize`.

## License

MIT
