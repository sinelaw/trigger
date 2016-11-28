#include "trigger.h"

extern "C" {
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "fshook/protocol.h"
}
#include <cstdlib>
#include <cinttypes>
#include <sstream>
#include <iostream>

#define MIN(x,y) (x < y ? x : y)


#define PROTOCOL_HELLO "PROTOCOL10: HELLO, I AM: "
#define LD_PRELOAD_PATH "./fs_override.so"



Trigger::Trigger(FileRequestCb *cb) {
    m_cb = cb;
    m_child_idx = 0;
    for (uint32_t i = 0; i < ARRAY_LEN(m_threads); i++) {
        m_threads[i].in_use = false;
        m_threads[i].running = false;
    }
    m_threads_lock = false;
}

static char putenv_buffers[1024][1024];
static uint32_t putenv_pos = 0;

#define PUTENV(fmt, ...) do {                                           \
        char *result = putenv_buffers[putenv_pos];                      \
        ASSERT(putenv_pos < ARRAY_LEN(putenv_buffers));                 \
        putenv_pos++;                                                   \
        const int32_t buf_size = ARRAY_LEN(putenv_buffers[0]) - 1;      \
        if (buf_size <= snprintf(result, buf_size, fmt, ##__VA_ARGS__)) { \
            PANIC("string too long: %s", fmt);                          \
        }                                                               \
        ASSERT(0 == putenv(result));                                    \
    } while (0);


static int trigger_listen(const char *addr, struct sockaddr_un *out_addr) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT(-1 != fd);
    *out_addr = (struct sockaddr_un){
        .sun_family = AF_UNIX,
        .sun_path = {0},
    };
    ASSERT(strlen(addr) < sizeof(out_addr->sun_path));
    strcpy(out_addr->sun_path, addr);

    LOG("Binding to: %s", out_addr->sun_path);
    ASSERT(0 == bind(fd,
                     (struct sockaddr *) out_addr,
                     sizeof(struct sockaddr_un)));
    ASSERT(0 == listen(fd, 5));
    return fd;
}

static bool send_go(int connection_fd) {
    LOG("GO");
    return (2 == send(connection_fd, "GO", 2, 0));
}

static bool checked_recv(int sockfd, void *buf, size_t len)
{
    char *pos = (char *)buf;
    uint32_t received_amount = 0;
    while (true) {
        int received = recv(sockfd, pos, len - received_amount, 0);
        if (received < 0) {
            perror("recv");
            LOG("recv returned: %d, errno: %d", received, errno);
            return false;
        }
        if (received == 0) {
            LOG("recv returned 0, socket closed?");
            return false;
        }
        pos += received;
        received_amount += (uint32_t)received;
        if (received_amount == len) {
            return true;
        }
        LOG("need moar bytes, have %u/%lu", received_amount, len);
    }
}

static bool recv_buf(int connection_fd, char *buf, uint32_t buf_size, uint32_t *out_received)
{
    uint32_t size_n;
    if (!checked_recv(connection_fd, &size_n, sizeof(size_n))) return false;
    const uint32_t size = htonl(size_n);
    *out_received = size;
    if (size == 0) return true;

    // LOG("recv size: " ,  size);
    ASSERT(buf_size > size);
    return checked_recv(connection_fd, buf, size);
}

static void debug_req(enum func func_id, bool delayed, uint32_t str_size)
{
    const char *name;
    switch (func_id) {
    case func_openr: name = "openr"; break;
    case func_openw: name = "openw"; break;
    case func_creat: name = "creat"; break;
    case func_stat: name = "stat"; break;
    case func_lstat: name = "lstat"; break;
    case func_opendir: name = "opendir"; break;
    case func_access: name = "access"; break;
    case func_truncate: name = "truncate"; break;
    case func_unlink: name = "unlink"; break;
    case func_rename: name = "rename"; break;
    case func_chmod: name = "chmod"; break;
    case func_readlink: name = "readlink"; break;
    case func_mknod: name = "mknod"; break;
    case func_mkdir: name = "mkdir"; break;
    case func_rmdir: name = "rmdir"; break;
    case func_symlink: name = "symlink"; break;
    case func_link: name = "link"; break;
    case func_chown: name = "chown"; break;
    case func_exec: name = "exec"; break;
    case func_execp: name = "execp"; break;
    case func_realpath: name = "realpath"; break;
    case func_trace: name ="trace"; break;
    default: PANIC("Invalid func_id: %u", func_id);
    }

    LOG("recv: delayed=%s, func=%s, size=%u",
        (delayed ? "yes" : "no"),  name, str_size);
    (void)name;
    (void)delayed;
    (void)str_size;
}

static bool wait_for(pid_t child) {
    int wait_res;
    int wait_child = waitpid(child, &wait_res, WNOHANG);
    if ((wait_child < 0) && (errno == EINTR)) {
        return false;
    }
    ASSERT(wait_child >= 0);
    if (wait_child == 0) {
        return false;
    }
    ASSERT(wait_child == child);
    return true;
}

struct ConnectionParams {
    Trigger *trigger;
    int connection_fd;
    volatile bool started;
    const struct TargetContext *target_ctx;
};

static __thread uint64_t connections_accepted = 0;
static __thread uint64_t connections_completed = 0;

void *thread_start(void *arg)
{
    struct ConnectionParams *orig_params = (struct ConnectionParams *)arg;
    const struct ConnectionParams params = *orig_params;
    orig_params->started = true;
    LOG("Handling: %d",  params.connection_fd);
    params.trigger->handle_connection(params.connection_fd, params.target_ctx);
    close(params.connection_fd);
    LOG("Terminating");
    return NULL;
}

void Trigger::harvest_threads()
{
    while (m_threads_lock) usleep(100);
    m_threads_lock = true;
    for (uint32_t i = 0; i < ARRAY_LEN(m_threads); i++) {
        if (!m_threads[i].in_use) continue;
        if (!m_threads[i].running) continue;
        void *res;
        if (0 == pthread_tryjoin_np(m_threads[i].thread_id, &res)) {
            m_threads[i].running = false;
            m_threads[i].in_use = false;
            connections_completed++;
            LOG("connections_accepted: %lu, connections_completed: %lu", connections_accepted, connections_completed);
        }
    }
    m_threads_lock = false;
}

struct Trigger::Thread *Trigger::alloc_thread()
{
    while (true) {
        while (m_threads_lock) usleep(100);
        m_threads_lock = true;
        for (uint32_t i = 0; i < ARRAY_LEN(m_threads); i++) {
            if (!m_threads[i].in_use) {
                m_threads[i].in_use = true;
                m_threads_lock = false;
                return &m_threads[i];
            }
            if (!m_threads[i].running) continue; /* started but not yet running, can't use it */
            void *res;
            if (0 == pthread_tryjoin_np(m_threads[i].thread_id, &res)) {
                m_threads[i].running = false;
                m_threads_lock = false;
                return &m_threads[i];
            }
        }
        m_threads_lock = false;
        usleep(100);
    }
}

bool Trigger::trigger_accept(int fd, const struct sockaddr_un *addr, const struct TargetContext *target_ctx)
{
    socklen_t addrlen = sizeof(struct sockaddr_un);
    int connection_fd;
    this->harvest_threads();
    // if (connections_accepted > 0 && (connections_completed == connections_accepted)) {
    //     LOG("All connections closed");
    //     return false;
    // }
    connection_fd = accept(fd, (struct sockaddr *)addr, &addrlen);
    if (connection_fd < 0) {
        ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);
        return false;
    }
    ASSERT(connection_fd >= 0);
    // HANDLE CONNECTION
    connections_accepted++;
    LOG("Got connection, connections_accepted: %lu", connections_accepted);

    Thread *const thread = this->alloc_thread();
    ASSERT(thread != nullptr);
    ASSERT(thread->running == false);
    thread->trigger = this;
    struct ConnectionParams params = {
        .trigger = this,
        .connection_fd = connection_fd,
        .started = false,
        .target_ctx = target_ctx,
    };
    ASSERT(0 == pthread_attr_init(&thread->attr));
    ASSERT(0 == pthread_create(&thread->thread_id, &thread->attr,
                               &thread_start, &params));
    ASSERT(0 == pthread_attr_destroy(&thread->attr));
    LOG("Waiting for handler thread: %lX", thread->thread_id);
    while (!params.started) {
        usleep(100);
    }
    thread->running = true;
    LOG("Done waiting for handler thread: %lX", thread->thread_id);
    return true;
}

const char *get_input_path(enum func func_id, const char *buf, uint32_t buf_size)
{
    LOG("func_id: %X", func_id);
    switch (func_id) {
    case func_openr: {
        DEFINE_DATA(struct func_openr, buf, buf_size, data);
        return data->path.in_path;
    }
    case func_stat: {
        DEFINE_DATA(struct func_stat, buf, buf_size, data);
        return data->path.in_path;
    }
    case func_lstat: {
        DEFINE_DATA(struct func_lstat, buf, buf_size, data);
        return data->path.in_path;
    }
    case func_opendir: {
        DEFINE_DATA(struct func_opendir, buf, buf_size, data);
        return data->path.in_path;
    }
    case func_access: {
        DEFINE_DATA(struct func_access, buf, buf_size, data);
        return data->path.in_path;
    }
    case func_readlink: {
        DEFINE_DATA(struct func_readlink, buf, buf_size, data);
        return data->path.in_path;
    }
    case func_symlink: {
        DEFINE_DATA(struct func_symlink, buf, buf_size, data);
        return data->linkpath.out_path; // TODO - out
    }
    case func_exec: {
        DEFINE_DATA(struct func_exec, buf, buf_size, data);
        return data->path.in_path;
    }
    case func_realpath: {
        DEFINE_DATA(struct func_realpath, buf, buf_size, data);
        return data->path.in_path;
    }
    case func_execp: {
        DEFINE_DATA(struct func_execp, buf, buf_size, data);
        return data->file;
    }
    case func_openw:
    case func_creat:
    case func_truncate:
    case func_unlink:
    case func_rename:
    case func_chmod:
    case func_mknod:
    case func_mkdir:
    case func_rmdir:
    case func_link:
    case func_chown:
        return "<TODO>";
    case func_trace: {
        DEFINE_DATA(struct func_trace, buf, buf_size, data);
        LOG("TRACE: %s", data->msg);
        return "<TODO>";
    }
    default: PANIC("Unknown command: %d", func_id);
    }
}

void Trigger::want(const char *input_path, const struct TargetContext *target_ctx)
{
    auto it = m_fileStatus.find(input_path);
    const struct TargetContext new_target_ctx = {
        .path = input_path,
        .parent = target_ctx,
    };
    bool is_nesting = false;;
    const struct TargetContext *cur = target_ctx;
    while (cur) {
        if (0 == strcmp(cur->path, input_path)) {
            is_nesting = true;
            break;
        }
        cur = cur->parent;
    }
    /* in child of parent building this path, just release it */
    if (is_nesting) {
        return;
    }
    if (it == m_fileStatus.end()) {
        m_fileStatus[input_path] = REQUESTED_FILE_STATUS_PENDING;
        m_cb(input_path, &new_target_ctx);
        m_fileStatus[input_path] = REQUESTED_FILE_STATUS_READY;
        return;
    }
    switch (it->second) {
    case REQUESTED_FILE_STATUS_READY: return;
    case REQUESTED_FILE_STATUS_UNKNOWN:
        m_fileStatus[input_path] = REQUESTED_FILE_STATUS_PENDING;
        m_cb(input_path, &new_target_ctx);
        m_fileStatus[input_path] = REQUESTED_FILE_STATUS_READY;
        break;
    case REQUESTED_FILE_STATUS_PENDING: {
        LOG("Waiting for: '%s'...", input_path);
        while (true) {
            auto it2 = m_fileStatus.find(input_path);
            ASSERT(it2 != m_fileStatus.end());
            if (it2->second == REQUESTED_FILE_STATUS_READY) {
                break;
            }
            usleep(100);
        }
        break;
    }
    }
}

void Trigger::handle_connection(int connection_fd, const struct TargetContext *target_ctx)
{
    char buf[0x8000];
    uint32_t size;
    if (!recv_buf(connection_fd, buf, sizeof(buf), &size)) return;
    if (0 != strncmp(PROTOCOL_HELLO, buf, MIN(size, strlen(PROTOCOL_HELLO)))) {
        PANIC("Exepcting HELLO message, got: %s", buf);
    }
    if (!send_go(connection_fd)) return;

    while (true) {
        if (!recv_buf(connection_fd, buf, sizeof(buf), &size)) break;
        const char *pos = buf;
        const bool delayed = *(const bool*)pos;
        pos += sizeof(delayed);
        const enum func func_id = *(enum func *)pos;
        pos += sizeof(func_id);
        const uint32_t str_size = size - (pos - buf);
        debug_req(func_id, delayed, str_size);

        const char *const input_path = get_input_path(func_id, pos, str_size);
        // LOG(input_path);
        if (!delayed) continue;
        this->want(input_path, target_ctx);
        if (!send_go(connection_fd)) break;
    }
}

void Trigger::Execute(const char *cmd, const struct TargetContext *target_ctx)//, char *const argv[])
{
    uint64_t child_idx = m_child_idx;
    m_child_idx++;

    std::ostringstream stringStream;
    stringStream << "/tmp/trigger.sock." << getpid() << "." << child_idx;
    const std::string sockAddr = stringStream.str();


    LOG("Forking child: %s", cmd);
    std::cerr << "Execute: " << cmd << std::endl;

    int parent_child_pipe[2];
    ASSERT(0 == pipe(parent_child_pipe));
    const pid_t child = fork();
    if (0 == child) {
        close(parent_child_pipe[1]);
        LOG("Waiting for parent...");

        bool yup;
        read(parent_child_pipe[0], &yup, sizeof(yup));
        LOG("Starting...");

        char *const cwd = get_current_dir_name();
        PUTENV("LD_PRELOAD=%s/%s", cwd, LD_PRELOAD_PATH);
        PUTENV("BUILDSOME_MASTER_UNIX_SOCKADDR=%s", sockAddr.c_str());
        PUTENV("BUILDSOME_JOB_ID=%lu", child_idx);
        PUTENV("BUILDSOME_ROOT_FILTER=%s", cwd);
        free(cwd);
        const char *const args[] = { SHELL_EXE_PATH, "-c", cmd, NULL };
        execvp("/bin/sh", (char *const*)args);
        PANIC("exec failed?!");
    }

    close(parent_child_pipe[0]);
    LOG("Forked child: %d", child);

    struct sockaddr_un addr;
    int sock_fd = trigger_listen(sockAddr.c_str(), &addr);

    const bool parent_yup = true;
    ASSERT(sizeof(parent_yup) == write(parent_child_pipe[1], &parent_yup, sizeof(parent_yup)));

    LOG("Sent yup");

    while (!wait_for(child)) {
        this->trigger_accept(sock_fd, &addr, target_ctx);
        usleep(100);
    }
    LOG("Done accepting, waiting for child: %d", child);
    LOG("Child %d terminated", child);
    close(sock_fd);

    std::cerr << "Execute [done]: " << cmd << std::endl;
}
