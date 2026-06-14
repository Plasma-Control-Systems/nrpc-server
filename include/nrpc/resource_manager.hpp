#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <type_traits>
#include <unordered_map>

namespace nrpc {

using ClientId = std::uint64_t;
using HandleId = std::uint64_t;

template <typename T>
concept NrpcHandle = requires(T& handle) {
    { handle.close() } noexcept -> std::same_as<void>;
} && (!std::copy_constructible<T>) && (!std::is_copy_assignable_v<T>);

class ResourceManager {
public:
    ResourceManager() = default;
    ~ResourceManager();
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    template <NrpcHandle T>
    bool register_handle(ClientId client_id, HandleId handle_id, std::unique_ptr<T> handle) {
        std::scoped_lock lock(mutex_);
        Key key{client_id, handle_id};
        return handles_.emplace(key, std::make_shared<Holder<T>>(std::move(handle))).second;
    }

    template <NrpcHandle T>
    T* find(ClientId client_id, HandleId handle_id) {
        std::scoped_lock lock(mutex_);
        auto iterator = handles_.find(Key{client_id, handle_id});
        if (iterator == handles_.end()) {
            return nullptr;
        }
        auto* holder = dynamic_cast<Holder<T>*>(iterator->second.get());
        return holder == nullptr ? nullptr : holder->value.get();
    }

    template <NrpcHandle T, typename Function>
    auto with_handle(ClientId client_id, HandleId handle_id, Function&& function)
        -> std::optional<std::invoke_result_t<Function, T&>> {
        std::shared_ptr<HolderBase> entry;
        {
            std::scoped_lock lock(mutex_);
            auto iterator = handles_.find(Key{client_id, handle_id});
            if (iterator == handles_.end()) {
                return std::nullopt;
            }
            entry = iterator->second;
        }
        auto* holder = dynamic_cast<Holder<T>*>(entry.get());
        if (holder == nullptr || holder->value == nullptr) {
            return std::nullopt;
        }
        return std::invoke(std::forward<Function>(function), *holder->value);
    }

    bool remove(ClientId client_id, HandleId handle_id);
    std::size_t cleanup_client(ClientId client_id);
    [[nodiscard]] std::size_t size() const;

private:
    struct Key {
        ClientId client_id;
        HandleId handle_id;
        bool operator==(const Key&) const = default;
    };

    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept;
    };

    struct HolderBase {
        virtual ~HolderBase() = default;
        virtual void close() noexcept = 0;
    };

    template <NrpcHandle T>
    struct Holder final : HolderBase {
        explicit Holder(std::unique_ptr<T> handle) : value(std::move(handle)) {}
        ~Holder() override { close(); }
        void close() noexcept override {
            if (value != nullptr) {
                value->close();
                value.reset();
            }
        }
        std::unique_ptr<T> value;
    };

    mutable std::mutex mutex_;
    std::unordered_map<Key, std::shared_ptr<HolderBase>, KeyHash> handles_;
};

}  // namespace nrpc
