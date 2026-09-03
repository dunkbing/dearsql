#pragma once

#include "acp/types.hpp"
#include <optional>
#include <string>

namespace acp {

    // The client side of the protocol: what the agent calls on you. Implement
    // this and hand it to Connection. All callbacks run on the connection's
    // reader thread; hop to your UI thread yourself if you need to.
    class Client {
    public:
        virtual ~Client() = default;

        virtual void sessionUpdate(const SessionNotification& notification) = 0;

        // answer later through Connection::respondPermission(request.rpcId, ...)
        virtual void requestPermission(const PermissionRequest& request) = 0;

        // fs/read_text_file and fs/write_text_file. nullopt = not supported,
        // which is what an agent gets when you did not advertise the capability
        virtual std::optional<Response> readTextFile(const json& /*params*/) {
            return std::nullopt;
        }
        virtual std::optional<Response> writeTextFile(const json& /*params*/) {
            return std::nullopt;
        }

        // terminal/* and extension requests. nullopt = method not supported
        virtual std::optional<Response> extRequest(const std::string& /*method*/,
                                                   const json& /*params*/) {
            return std::nullopt;
        }
        virtual void extNotification(const std::string& /*method*/, const json& /*params*/) {}

        // process lifecycle
        virtual void agentStderr(const std::string& /*line*/) {}
        virtual void agentExited() {}
    };

} // namespace acp
