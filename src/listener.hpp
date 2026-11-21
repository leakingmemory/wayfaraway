#pragma once

#include <functional>
#include <utility>

#include <wayland-server-core.h>

namespace wfa {

// RAII wrapper around wl_listener that dispatches to a std::function.
// Disconnects automatically on destruction, so owning objects can simply hold
// Listener members and never leak a dangling link into a wl_signal.
class Listener {
public:
    using Callback = std::function<void(void*)>;

    Listener()
    {
        m_hook.raw.notify = &Listener::dispatch;
        m_hook.owner = this;
        wl_list_init(&m_hook.raw.link);
    }

    ~Listener() { disconnect(); }

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) = delete;
    Listener& operator=(Listener&&) = delete;

    void connect(wl_signal* signal, Callback callback)
    {
        disconnect();
        m_callback = std::move(callback);
        wl_signal_add(signal, &m_hook.raw);
    }

    void disconnect()
    {
        wl_list_remove(&m_hook.raw.link);
        wl_list_init(&m_hook.raw.link);
    }

private:
    struct Hook {
        wl_listener raw;
        Listener* owner;
    };

    static void dispatch(wl_listener* raw, void* data)
    {
        // `raw` is the first member of the standard-layout Hook.
        reinterpret_cast<Hook*>(raw)->owner->m_callback(data);
    }

    Hook m_hook{};
    Callback m_callback;
};

} // namespace wfa
