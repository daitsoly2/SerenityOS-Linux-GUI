/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "detail/IndependentState.h"

namespace WsWlrBridge::detail {

// ── HandleMap ────────────────────────────────────────────────────────────────
// [独立] 只做双向查表；generation 用来让 unmap→remap 之后的旧 id 不再复活。
void HandleMap::bind(WlrSurfaceHandle surface, WsWindowId id)
{
    auto generation = m_next_generation++;
    // 同一个 surface 重新绑定时，先撤掉旧映射，避免 by_id 里留下孤儿。
    auto old = m_by_surface.find(surface);
    if (old != m_by_surface.end()) {
        m_by_id.erase(old->second.id);
        m_generations.erase(old->second.id);
    }
    m_by_surface[surface] = { id, generation };
    m_by_id[id] = surface;
    m_generations[id] = generation;
}

void HandleMap::unbind(WlrSurfaceHandle surface)
{
    auto it = m_by_surface.find(surface);
    if (it == m_by_surface.end())
        return;
    m_by_id.erase(it->second.id);
    m_generations.erase(it->second.id);
    m_by_surface.erase(it);
}

WsWindowId HandleMap::find_by_surface(WlrSurfaceHandle surface) const
{
    auto it = m_by_surface.find(surface);
    return it == m_by_surface.end() ? kInvalidWindowId : it->second.id;
}

WlrSurfaceHandle HandleMap::find_by_id(WsWindowId id) const
{
    auto it = m_by_id.find(id);
    return it == m_by_id.end() ? nullptr : it->second;
}

uint32_t HandleMap::generation_of(WsWindowId id) const
{
    auto it = m_generations.find(id);
    return it == m_generations.end() ? 0 : it->second;
}

// ── SerialMap ────────────────────────────────────────────────────────────────
void SerialMap::remember(WlrSerial serial, WsWindowId window) { m_by_serial[serial] = window; }

bool SerialMap::resolve(WlrSerial serial, WsWindowId& out) const
{
    auto it = m_by_serial.find(serial);
    if (it == m_by_serial.end())
        return false;
    out = it->second;
    return true;
}

void SerialMap::erase(WlrSerial serial) { m_by_serial.erase(serial); }

// ── PendingQueue ─────────────────────────────────────────────────────────────
// [独立] 严格 FIFO。刻意不做合并/去重：那是窗口策略或优化，桥无权决定。
void PendingQueue::push(WlrSurfaceHandle surface, PendingEvent event)
{
    m_queues[surface].push_back(std::move(event));
}

std::vector<PendingEvent> PendingQueue::take(WlrSurfaceHandle surface)
{
    auto it = m_queues.find(surface);
    if (it == m_queues.end())
        return {};
    auto events = std::move(it->second);
    m_queues.erase(it);
    return events;
}

void PendingQueue::requeue_front(WlrSurfaceHandle surface, std::vector<PendingEvent> const& events)
{
    if (events.empty())
        return;
    auto& queue = m_queues[surface];
    queue.insert(queue.begin(), events.begin(), events.end());
}

size_t PendingQueue::count(WlrSurfaceHandle surface) const
{
    auto it = m_queues.find(surface);
    return it == m_queues.end() ? 0 : it->second.size();
}

size_t PendingQueue::total() const
{
    size_t sum = 0;
    for (auto const& entry : m_queues)
        sum += entry.second.size();
    return sum;
}

// ── StateMirror ──────────────────────────────────────────────────────────────
WindowRecord& StateMirror::ensure(WlrSurfaceHandle surface)
{
    auto it = m_windows.find(surface);
    if (it == m_windows.end())
        it = m_windows.emplace(surface, WindowRecord {}).first;
    return it->second;
}

WindowRecord* StateMirror::find(WlrSurfaceHandle surface)
{
    auto it = m_windows.find(surface);
    return it == m_windows.end() ? nullptr : &it->second;
}

WindowRecord const* StateMirror::find(WlrSurfaceHandle surface) const
{
    auto it = m_windows.find(surface);
    return it == m_windows.end() ? nullptr : &it->second;
}

void StateMirror::erase(WlrSurfaceHandle surface) { m_windows.erase(surface); }

void StateMirror::expect_release(WsCookie cookie, ReleaseTicket ticket)
{
    m_releases[cookie] = ticket;
}

bool StateMirror::take_release(WsCookie cookie, ReleaseTicket& out)
{
    auto it = m_releases.find(cookie);
    if (it == m_releases.end())
        return false;
    out = it->second;
    m_releases.erase(it);
    return true;
}

std::vector<ReleaseTicket> StateMirror::take_releases_for(WlrSurfaceHandle surface)
{
    std::vector<ReleaseTicket> tickets;
    for (auto it = m_releases.begin(); it != m_releases.end();) {
        if (it->second.surface == surface) {
            tickets.push_back(it->second);
            it = m_releases.erase(it);
        } else {
            ++it;
        }
    }
    return tickets;
}

size_t StateMirror::mapped_count() const
{
    size_t count = 0;
    for (auto const& entry : m_windows) {
        if (entry.second.id != kInvalidWindowId && entry.second.lifecycle == Lifecycle::Mapped)
            ++count;
    }
    return count;
}

size_t StateMirror::pending_release_count() const { return m_releases.size(); }

}
