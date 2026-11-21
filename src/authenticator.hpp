#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct wl_event_loop;
struct wl_event_source;

namespace wfa {

// Verifies a password against PAM without blocking the compositor: the PAM
// conversation runs on a worker thread and the result is delivered back on
// the Wayland event loop through an eventfd.
class Authenticator {
public:
    using ResultCallback = std::function<void(bool success, std::string message)>;

    Authenticator(wl_event_loop* loop, std::string service, std::string user);
    ~Authenticator();

    Authenticator(const Authenticator&) = delete;
    Authenticator& operator=(const Authenticator&) = delete;

    const std::string& user() const { return m_user; }
    bool busy() const { return m_busy; }

    // Takes ownership of `password` and wipes it once PAM is done with it.
    // Ignored while a previous attempt is still running.
    void authenticate(std::string password, ResultCallback callback);

private:
    static int on_eventfd(int fd, uint32_t mask, void* data);
    void run(std::string password);

    std::string m_service;
    std::string m_user;

    int m_eventfd = -1;
    wl_event_source* m_source = nullptr;

    bool m_busy = false;
    ResultCallback m_callback;
    std::jthread m_worker;

    std::mutex m_mutex;
    bool m_result_success = false;
    std::string m_result_message;
};

} // namespace wfa
