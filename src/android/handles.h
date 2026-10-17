// Opaque 32-bit handles for host pointers the guest never dereferences
// (ANativeWindow*, AInputEvent*, EGLDisplay, ...).
#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>

#include "../common.h"

namespace zb {

template <typename T>
class PtrMap {
public:
    explicit PtrMap(u32 tag) : tag_(tag & 0xFF) {}

    u32 to_guest(T* p) {
        if (!p) return 0;
        std::lock_guard lk(m_);
        if (auto it = fwd_.find(p); it != fwd_.end()) return it->second;
        u32 idx;
        if (!free_.empty()) {
            idx = free_.back();
            free_.pop_back();
            back_[idx] = p;
        } else {
            idx = static_cast<u32>(back_.size());
            back_.push_back(p);
        }
        u32 h = ((idx + 1) << 8) | tag_;
        fwd_[p] = h;
        return h;
    }

    // Like to_host, but a stale handle is an expected answer: nothing is logged.
    T* find_host(u32 h) {
        if (!h) return nullptr;
        std::lock_guard lk(m_);
        u32 idx = (h >> 8) - 1;
        if ((h & 0xFF) != tag_ || idx >= back_.size()) return nullptr;
        return back_[idx];
    }

    T* to_host(u32 h) {
        if (!h) return nullptr;
        std::lock_guard lk(m_);
        u32 idx = (h >> 8) - 1;
        if ((h & 0xFF) != tag_ || idx >= back_.size() || !back_[idx]) {
            ZLOGE("invalid handle 0x%08x (tag 0x%02x)", h, tag_);
            return nullptr;
        }
        return back_[idx];
    }

    void remove(T* p) {
        if (!p) return;
        std::lock_guard lk(m_);
        auto it = fwd_.find(p);
        if (it == fwd_.end()) return;
        u32 idx = (it->second >> 8) - 1;
        back_[idx] = nullptr;
        free_.push_back(idx);
        fwd_.erase(it);
    }

private:
    std::mutex m_;
    u32 tag_;
    std::unordered_map<T*, u32> fwd_;
    std::vector<T*> back_;
    std::vector<u32> free_;
};

}  // namespace zb
