#include "authenticator.hpp"

#include <cstdlib>
#include <cstring>
#include <format>
#include <utility>

#include <security/pam_appl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "wlr.hpp"

namespace wfa {

namespace {

struct Conversation {
    const char* password;
    std::string message;
};

int converse(int count, const pam_message** messages, pam_response** responses, void* data)
{
    auto* conv = static_cast<Conversation*>(data);

    auto* replies = static_cast<pam_response*>(std::calloc(count, sizeof(pam_response)));
    if (!replies)
        return PAM_BUF_ERR;

    for (int i = 0; i < count; ++i) {
        switch (messages[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            replies[i].resp = strdup(conv->password);
            if (!replies[i].resp) {
                for (int j = 0; j < i; ++j)
                    std::free(replies[j].resp);
                std::free(replies);
                return PAM_BUF_ERR;
            }
            break;
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            if (messages[i]->msg)
                conv->message = messages[i]->msg;
            break;
        default:
            break;
        }
    }

    *responses = replies;
    return PAM_SUCCESS;
}

} // namespace

Authenticator::Authenticator(wl_event_loop* loop, std::string service, std::string user)
    : m_service(std::move(service))
    , m_user(std::move(user))
{
    m_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_eventfd < 0) {
        wlr_log_errno(WLR_ERROR, "eventfd failed; unlocking will be impossible");
        return;
    }
    m_source = wl_event_loop_add_fd(loop, m_eventfd, WL_EVENT_READABLE, &Authenticator::on_eventfd, this);
}

Authenticator::~Authenticator()
{
    if (m_worker.joinable())
        m_worker.join();
    if (m_source)
        wl_event_source_remove(m_source);
    if (m_eventfd >= 0)
        close(m_eventfd);
}

void Authenticator::authenticate(std::string password, ResultCallback callback)
{
    if (m_busy || !m_source) {
        explicit_bzero(password.data(), password.size());
        return;
    }

    if (m_worker.joinable())
        m_worker.join();

    m_busy = true;
    m_callback = std::move(callback);
    m_worker = std::jthread([this, password = std::move(password)]() mutable { run(std::move(password)); });
}

void Authenticator::run(std::string password)
{
    Conversation conv{ password.c_str(), {} };
    pam_conv pam_conversation{ &converse, &conv };
    pam_handle_t* handle = nullptr;

    int status = pam_start(m_service.c_str(), m_user.c_str(), &pam_conversation, &handle);
    if (status == PAM_SUCCESS)
        status = pam_authenticate(handle, 0);
    if (status == PAM_SUCCESS)
        status = pam_acct_mgmt(handle, 0);

    std::string message = conv.message;
    if (status != PAM_SUCCESS && message.empty())
        message = handle ? pam_strerror(handle, status) : "PAM initialisation failed";

    if (handle)
        pam_end(handle, status);

    explicit_bzero(password.data(), password.size());

    {
        std::scoped_lock lock(m_mutex);
        m_result_success = status == PAM_SUCCESS;
        m_result_message = std::move(message);
    }

    std::uint64_t one = 1;
    [[maybe_unused]] auto n = write(m_eventfd, &one, sizeof(one));
}

int Authenticator::on_eventfd(int fd, uint32_t mask, void* data)
{
    auto* self = static_cast<Authenticator*>(data);

    std::uint64_t value = 0;
    [[maybe_unused]] auto n = read(fd, &value, sizeof(value));

    if (self->m_worker.joinable())
        self->m_worker.join();

    bool success = false;
    std::string message;
    {
        std::scoped_lock lock(self->m_mutex);
        success = self->m_result_success;
        message = std::move(self->m_result_message);
    }

    self->m_busy = false;
    if (auto callback = std::exchange(self->m_callback, nullptr))
        callback(success, std::move(message));
    return 0;
}

} // namespace wfa
