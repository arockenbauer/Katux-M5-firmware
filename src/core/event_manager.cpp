#include "event_manager.h"

namespace katux::core {

void EventManager::begin() {
    clear();
}

bool EventManager::push(const Event& event) {
    if (count_ >= kQueueSize) {
        return false;
    }
    queue_[tail_] = event;
    tail_ = static_cast<uint8_t>((tail_ + 1U) % kQueueSize);
    ++count_;
    return true;
}

bool EventManager::pop(Event& event) {
    if (count_ == 0) {
        return false;
    }
    event = queue_[head_];
    head_ = static_cast<uint8_t>((head_ + 1U) % kQueueSize);
    --count_;
    return true;
}

bool EventManager::peek(Event& event) const {
    if (count_ == 0) {
        return false;
    }
    event = queue_[head_];
    return true;
}

uint8_t EventManager::size() const {
    return count_;
}

bool EventManager::empty() const {
    return count_ == 0;
}

void EventManager::clear() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

}