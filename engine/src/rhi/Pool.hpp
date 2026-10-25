#pragma once

// The table every backend keeps its resources in.

#include <cstdint>
#include <utility>
#include <vector>

namespace fitzel::rhi {

// Handles are slots in one of these: index + 1, so that 0 stays "none". Freed
// slots are reused, as OpenGL reuses names.
template <class T>
class Pool {
public:
    std::uint32_t add(T item) {
        if (!m_free.empty()) {
            const std::uint32_t i = m_free.back();
            m_free.pop_back();
            m_items[i] = std::move(item);
            m_live[i] = true;
            return i + 1;
        }
        m_items.push_back(std::move(item));
        m_live.push_back(true);
        return static_cast<std::uint32_t>(m_items.size());
    }
    T* find(std::uint32_t id) {
        if (id == 0 || id > m_items.size() || !m_live[id - 1]) return nullptr;
        return &m_items[id - 1];
    }
    const T* find(std::uint32_t id) const {
        if (id == 0 || id > m_items.size() || !m_live[id - 1]) return nullptr;
        return &m_items[id - 1];
    }
    void remove(std::uint32_t id) {
        if (!find(id)) return;
        m_items[id - 1] = T{};
        m_live[id - 1] = false;
        m_free.push_back(id - 1);
    }
    // Every live slot, for a backend that has to free them all at the end.
    template <class F>
    void forEach(F&& f) {
        for (std::size_t i = 0; i < m_items.size(); ++i)
            if (m_live[i]) f(static_cast<std::uint32_t>(i + 1), m_items[i]);
    }

private:
    std::vector<T>             m_items;
    std::vector<bool>          m_live;
    std::vector<std::uint32_t> m_free;
};

} // namespace fitzel::rhi
