#pragma once

#include "../core/event_manager.h"

namespace katux::apps {

class AppBase {
   public:
    virtual ~AppBase() = default;
    virtual void onStart() = 0;
    virtual void onEvent(const katux::core::Event& e) = 0;
    virtual void render() = 0;
    virtual void onClose() = 0;
};

}  // namespace katux::apps
