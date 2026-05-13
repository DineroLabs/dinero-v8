#pragma once
#include <string_view>
#include <memory>
#include <nlohmann/json.hpp>

/**
 * @brief Clean event publishing interface
 * 
 * Allows components to publish events without knowing about transport details.
 * MiningEngine publishes events; WebSocket hub decides how to deliver them.
 */
struct IEventSink {
    virtual ~IEventSink() = default;
    virtual void publish(std::string_view topic, const nlohmann::json& payload) = 0;
};

using EventSinkPtr = std::shared_ptr<IEventSink>;
