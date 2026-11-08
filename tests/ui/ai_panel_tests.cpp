#include "ai/acp_agents.hpp"
#include "application.hpp"
#include "database/s3.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "ui/markdown_text.hpp"
#include "ui/tab_manager.hpp"
#include "utils/db_mcp_server.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>

// Tests for the assistant chat tab. These drive the real widgets by
// id, so they cover the things a build cannot: that the tab renders, that the
// input exists, and that typing '@' opens the context picker.
namespace {
    // api backend spawns no agent, mcp server or registry fetch under test
    void openAssistantTab(ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.getAppState()->setSetting("ai_sidebar_backend", "api");
        // a chat tab normally opens from a connection's context menu; unbound works too
        const auto tab = app.getTabManager()->createAIChatTab(nullptr);
        ctx->Yield(2);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->Yield(2);
    }
} // namespace

void RegisterAiPanelTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = nullptr;

    // the chat's MCP server hands S3 connections to the agent: real http, real bucket
    t = IM_REGISTER_TEST(engine, "Assistant", "Agent tools reach S3");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* endpoint = std::getenv("DEARSQL_TEST_S3_ENDPOINT");
        if (!endpoint) {
            ctx->LogWarning("set DEARSQL_TEST_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY");
            return;
        }
        DatabaseConnectionInfo info;
        info.type = DatabaseType::S3;
        info.name = "Bucket Store";
        const std::string ep = endpoint;
        info.host = ep.substr(0, ep.rfind(':'));
        info.port = std::stoi(ep.substr(ep.rfind(':') + 1));
        info.sslmode = SslMode::Disable;
        info.username = std::getenv("DEARSQL_TEST_S3_ACCESS_KEY");
        info.password = std::getenv("DEARSQL_TEST_S3_SECRET_KEY");
        auto db = std::make_shared<S3Database>(info);
        IM_CHECK_NO_RET(db->connect().first);
        const std::string bucket = "dearsql-mcp-ui-test";
        try {
            db->client()->createBucket(bucket);
        } catch (const std::exception&) {
        }

        DbMcpServer server;
        IM_CHECK_NO_RET(server.start());
        server.setConnections({{info.name, "s3", true, nullptr, db->client()}});
        httplib::Client http(server.url().substr(0, server.url().rfind('/')));
        auto call = [&](const nlohmann::json& body) {
            auto r = http.Post("/mcp",
                               {{"Authorization", "Bearer " + server.token()},
                                {"Accept", "application/json, text/event-stream"}},
                               body.dump(), "application/json");
            return r ? r->body : std::string();
        };
        const std::string list = call({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
        IM_CHECK(list.find("list_objects") != std::string::npos);
        const std::string buckets =
            call({{"jsonrpc", "2.0"},
                  {"id", 2},
                  {"method", "tools/call"},
                  {"params", {{"name", "list_objects"}, {"arguments", nlohmann::json::object()}}}});
        IM_CHECK(buckets.find(bucket) != std::string::npos);
        server.stop();
        db->client()->deleteBucket(bucket);
    };

    t = IM_REGISTER_TEST(engine, "Assistant", "Open tab");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistantTab(ctx);
        // the input only exists once the panel is up
        IM_CHECK(ctx->ItemExists("**/##ai_side_text"));
    };

    t = IM_REGISTER_TEST(engine, "Assistant", "At sign opens context picker");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistantTab(ctx);
        ctx->ItemClick("**/##ai_side_text");
        ctx->KeyChars("@");
        ctx->Yield(3);

        // the picker is a window of its own, opened only while '@' is pending
        IM_CHECK(ImGui::FindWindowByName("##ai_mention_popup") != nullptr);
    };

    t = IM_REGISTER_TEST(engine, "Assistant", "Slash opens command picker");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistantTab(ctx);
        ctx->ItemClick("**/##ai_side_text");
        ctx->KeyChars("/");
        ctx->Yield(3);

        IM_CHECK(ImGui::FindWindowByName("##ai_mention_popup") != nullptr);
    };

    // a gui app opened from the dock has a minimal PATH, so a bare name would not
    // spawn: every resolved agent command must start with a full path
    t = IM_REGISTER_TEST(engine, "Assistant", "Agent commands resolve to full paths");
    t->TestFunc = [](ImGuiTestContext*) {
        IM_CHECK_STR_EQ(AcpAgents::locateExecutable("sh").value_or("").c_str(), "/bin/sh");
        for (const auto& def : AcpAgents::catalog()) {
            if (const auto argv = AcpAgents::resolveInvocation(def)) {
                IM_CHECK(!argv->empty() && argv->front().starts_with("/"));
            }
        }
    };
}

void RegisterCodeBlockTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Assistant", "Code block copy icon copies the code");
    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(520, 260), ImGuiCond_Always);
        ImGui::Begin("md_code_test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        MarkdownText::render(
            "Here:\n\n```sql\nSELECT id, name\nFROM users -- all\nWHERE id = 42;\n```", "c");
        ImGui::End();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->Yield(2);
        ImGui::SetClipboardText("");
        ctx->ItemClick("**/###md_cp_c_0"); // the first code block
        IM_CHECK_STR_EQ(ImGui::GetClipboardText(),
                        "SELECT id, name\nFROM users -- all\nWHERE id = 42;");
    };
}
