// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include <limits>

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace hdpbrt {

static double _Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

PbrtSession::PbrtSession()
{
    _listenFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (_listenFd < 0) {
        perror("hdPbrt: socket");
        return;
    }
    int one = 1;
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(_listenFd, (sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(_listenFd, 4) != 0) {
        perror("hdPbrt: bind/listen");
        close(_listenFd);
        _listenFd = -1;
        return;
    }
    socklen_t len = sizeof(addr);
    getsockname(_listenFd, (sockaddr *)&addr, &len);
    _port = ntohs(addr.sin_port);
    _thread = std::thread([this]() { _ServerLoop(); });
}

PbrtSession::~PbrtSession()
{
    Stop();
    _quit = true;
    if (_thread.joinable())
        _thread.join();
    if (_listenFd >= 0)
        close(_listenFd);
}

double PbrtSession::ElapsedSeconds() const
{
    return _startTime > 0 ? _Now() - _startTime : 0.0;
}

bool PbrtSession::Start(const std::string &pbrtExe, const std::string &sceneFile,
                        const std::string &outputPfm, const std::string &logFile,
                        int width, int height, int nThreads)
{
    Stop();
    if (_listenFd < 0)
        return false;

    {
        std::lock_guard<std::mutex> lock(_imageMutex);
        _width = width;
        _height = height;
        _image.assign(size_t(width) * height * 3, 0.f);
        _depth.assign(size_t(width) * height, std::numeric_limits<float>::infinity());
        _primIds.assign(size_t(width) * height, -1.f);
        _instanceIds.assign(size_t(width) * height, -1.f);
        _alpha.assign(size_t(width) * height, 1.f);
        _hasData = false;
        _hasDepth = false;
        _hasIds = false;
        _hasAlpha = false;
    }
    _outputPfm = outputPfm;
    _logFile = logFile;
    unlink(outputPfm.c_str());

    std::string display = "127.0.0.1:" + std::to_string(_port);
    std::string threads = std::to_string(nThreads > 0 ? nThreads : 0);
    std::vector<std::string> args = {pbrtExe, "--display-server", display};
    if (nThreads > 0) {
        args.push_back("--nthreads");
        args.push_back(threads);
    }
    args.push_back(sceneFile);
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    // Environment: the host's library path (Houdini/Blender bundled libs) must
    // not leak into pbrt, which links the system OpenEXR.
    std::vector<std::string> envs;
    for (char **e = environ; e && *e; ++e) {
        if (strncmp(*e, "LD_LIBRARY_PATH=", 16) == 0 ||
            strncmp(*e, "LD_PRELOAD=", 11) == 0 ||
            strncmp(*e, "PBRT_PARENT_PID=", 16) == 0)
            continue;
        envs.emplace_back(*e);
    }
    envs.push_back("PBRT_PARENT_PID=" + std::to_string(getpid()));
    std::vector<char *> envp;
    for (auto &e : envs)
        envp.push_back(const_cast<char *>(e.c_str()));
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, logFile.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // Reset signal mask and default handlers so pbrt can be killed cleanly.
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::lock_guard<std::mutex> lock(_procMutex);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, pbrtExe.c_str(), &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "hdPbrt: failed to start %s: %s\n", pbrtExe.c_str(),
                strerror(rc));
        _failed = true;
        _finished = true;
        return false;
    }
    _failed = false;
    _finished = false;
    _startTime = _Now();
    _pid = pid;
    return true;
}

void PbrtSession::Stop()
{
    std::lock_guard<std::mutex> lock(_procMutex);
    pid_t pid = _pid.load();
    if (pid > 0) {
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        _pid = 0;
    }
    _finished = true;
}

void PbrtSession::_Reap(bool block)
{
    std::lock_guard<std::mutex> lock(_procMutex);
    pid_t pid = _pid.load();
    if (pid <= 0)
        return;
    int status = 0;
    pid_t r = waitpid(pid, &status, block ? 0 : WNOHANG);
    if (r != pid)
        return;
    _pid = 0;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok)
        ok = _LoadPfm(_outputPfm);
    if (!ok) {
        _failed = true;
        fprintf(stderr, "hdPbrt: pbrt failed (status %d); see %s\n", status,
                _logFile.c_str());
        // Echo the end of the log so the error shows up in the host console.
        if (FILE *f = fopen(_logFile.c_str(), "rb")) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, size > 2000 ? size - 2000 : 0, SEEK_SET);
            char buf[2001];
            size_t n = fread(buf, 1, 2000, f);
            buf[n] = 0;
            fprintf(stderr, "%s\n", buf);
            fclose(f);
        }
    }
    _finished = true;
}

bool PbrtSession::_LoadPfm(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char magic[3] = {0};
    int w = 0, h = 0;
    float scale = 0;
    bool ok = fscanf(f, "%2s %d %d %f", magic, &w, &h, &scale) == 4 &&
              strcmp(magic, "PF") == 0 && w > 0 && h > 0;
    if (ok)
        fgetc(f);  // single whitespace after header
    std::vector<float> data;
    if (ok) {
        data.resize(size_t(w) * h * 3);
        ok = fread(data.data(), sizeof(float), data.size(), f) == data.size();
    }
    fclose(f);
    if (!ok)
        return false;
    if (scale > 0) {  // big endian
        for (float &v : data) {
            uint32_t u;
            memcpy(&u, &v, 4);
            u = __builtin_bswap32(u);
            memcpy(&v, &u, 4);
        }
    }
    std::lock_guard<std::mutex> lock(_imageMutex);
    if (w != _width || h != _height)
        return false;
    // PFM rows go bottom to top.
    for (int y = 0; y < h; ++y)
        memcpy(&_image[size_t(y) * w * 3], &data[size_t(h - 1 - y) * w * 3],
               sizeof(float) * w * 3);
    _hasData = true;
    ++_version;
    return true;
}

bool PbrtSession::Snapshot(std::vector<float> *out, int *width, int *height,
                           uint64_t *version, std::vector<float> *depth,
                           std::vector<float> *primIds,
                           std::vector<float> *instanceIds,
                           std::vector<float> *alpha) const
{
    std::lock_guard<std::mutex> lock(_imageMutex);
    if (!_hasData || *version == _version)
        return false;
    *out = _image;
    if (depth) {
        if (_hasDepth)
            *depth = _depth;
        else
            depth->clear();
    }
    if (primIds) {
        if (_hasIds)
            *primIds = _primIds;
        else
            primIds->clear();
    }
    if (instanceIds) {
        if (_hasIds)
            *instanceIds = _instanceIds;
        else
            instanceIds->clear();
    }
    if (alpha) {
        if (_hasAlpha)
            *alpha = _alpha;
        else
            alpha->clear();
    }
    *width = _width;
    *height = _height;
    *version = _version;
    return true;
}

// --- display server -------------------------------------------------------

static bool _ReadString(const uint8_t *&p, const uint8_t *end, std::string *s)
{
    const uint8_t *z = (const uint8_t *)memchr(p, 0, end - p);
    if (!z)
        return false;
    s->assign((const char *)p, z - p);
    p = z + 1;
    return true;
}

template <typename T>
static bool _Read(const uint8_t *&p, const uint8_t *end, T *v)
{
    if (end - p < (ptrdiff_t)sizeof(T))
        return false;
    memcpy(v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

void PbrtSession::_HandleMessage(const std::vector<uint8_t> &msg)
{
    const uint8_t *p = msg.data() + 4, *end = msg.data() + msg.size();
    uint8_t type = 0, grab = 0;
    if (!_Read(p, end, &type) || type != 3 /* UpdateImage */)
        return;
    std::string title, channel;
    if (!_Read(p, end, &grab) || !_ReadString(p, end, &title) ||
        !_ReadString(p, end, &channel))
        return;
    // Title is "<film filename> (<pid>)": ignore stale processes.
    size_t open = title.rfind('(');
    if (open == std::string::npos)
        return;
    pid_t pid = (pid_t)atoi(title.c_str() + open + 1);
    if (pid != _pid.load() || _finished.load())
        return;
    int c = channel == "R"   ? 0
            : channel == "G" ? 1
            : channel == "B" ? 2
            : channel == "Z" ? 3
            : channel == "primId"     ? 4
            : channel == "instanceId" ? 5
            : channel == "A"          ? 6
                                      : -1;
    if (c < 0)
        return;
    int x, y, w, h;
    if (!_Read(p, end, &x) || !_Read(p, end, &y) || !_Read(p, end, &w) ||
        !_Read(p, end, &h))
        return;
    if ((size_t)(end - p) < size_t(w) * h * sizeof(float))
        return;
    std::lock_guard<std::mutex> lock(_imageMutex);
    if (x < 0 || y < 0 || x + w > _width || y + h > _height)
        return;
    const float *v = (const float *)p;  // may be unaligned; copy per value
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            float f;
            memcpy(&f, (const uint8_t *)v + (size_t(j) * w + i) * 4, 4);
            const size_t pix = size_t(y + j) * _width + (x + i);
            if (c == 3)
                _depth[pix] = f;
            else if (c == 4)
                _primIds[pix] = f;
            else if (c == 5)
                _instanceIds[pix] = f;
            else if (c == 6)
                _alpha[pix] = f;
            else
                _image[pix * 3 + c] = f;
        }
    if (c >= 3) {
        if (c == 3)
            _hasDepth = true;
        else if (c == 6)
            _hasAlpha = true;
        else
            _hasIds = true;
        return;  // the RGB channels bump the version
    }
    _hasData = true;
    ++_version;
}

void PbrtSession::_ServerLoop()
{
    int conn = -1;
    std::vector<uint8_t> buf;
    while (!_quit) {
        pollfd fds[2];
        int n = 0;
        fds[n++] = {_listenFd, POLLIN, 0};
        if (conn >= 0)
            fds[n++] = {conn, POLLIN, 0};
        int r = poll(fds, n, 100);
        if (r > 0 && (fds[0].revents & POLLIN)) {
            int c = accept4(_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (c >= 0) {
                if (conn >= 0)
                    close(conn);
                conn = c;
                buf.clear();
            }
        }
        if (r > 0 && n > 1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t tmp[1 << 16];
            ssize_t got = recv(conn, tmp, sizeof(tmp), 0);
            if (got <= 0) {
                close(conn);
                conn = -1;
                buf.clear();
            } else {
                buf.insert(buf.end(), tmp, tmp + got);
                size_t off = 0;
                while (buf.size() - off >= 4) {
                    int32_t len;
                    memcpy(&len, &buf[off], 4);
                    if (len < 5 || len > (1 << 28)) {  // garbage: drop connection
                        close(conn);
                        conn = -1;
                        off = buf.size();
                        break;
                    }
                    if (buf.size() - off < (size_t)len)
                        break;
                    std::vector<uint8_t> msg(buf.begin() + off, buf.begin() + off + len);
                    _HandleMessage(msg);
                    off += len;
                }
                buf.erase(buf.begin(), buf.begin() + std::min(off, buf.size()));
            }
        }
        if (_pid.load() > 0)
            _Reap(false);
    }
    if (conn >= 0)
        close(conn);
}

} // namespace hdpbrt
