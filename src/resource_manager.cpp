#include "nrpc/resource_manager.hpp"

#include <vector>

namespace nrpc {

ResourceManager::~ResourceManager() {
    std::scoped_lock lock(mutex_);
    handles_.clear();
}

std::size_t ResourceManager::KeyHash::operator()(const Key& key) const noexcept {
    const auto first = std::hash<ClientId>{}(key.client_id);
    const auto second = std::hash<HandleId>{}(key.handle_id);
    return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
}

bool ResourceManager::remove(ClientId client_id, HandleId handle_id) {
    std::shared_ptr<HolderBase> removed;
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = handles_.find(Key{client_id, handle_id});
        if (iterator == handles_.end()) {
            return false;
        }
        removed = std::move(iterator->second);
        handles_.erase(iterator);
    }
    return true;
}

std::size_t ResourceManager::cleanup_client(ClientId client_id) {
    std::vector<std::shared_ptr<HolderBase>> removed;
    {
        std::scoped_lock lock(mutex_);
        for (auto iterator = handles_.begin(); iterator != handles_.end();) {
            if (iterator->first.client_id == client_id) {
                removed.push_back(std::move(iterator->second));
                iterator = handles_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    return removed.size();
}

std::size_t ResourceManager::size() const {
    std::scoped_lock lock(mutex_);
    return handles_.size();
}

}  // namespace nrpc
