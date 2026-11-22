// hdPbrt: runs pbrt-v4 as a child process and receives its progressive image
// through pbrt's display-server (tev) protocol on a localhost socket.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef HDPBRT_SESSION_H
#define HDPBRT_SESSION_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>

namespace hdpbrt {

class PbrtSession {
public:
    PbrtSession();
    ~PbrtSession();

    // Kill any running render, then start pbrt on sceneFile. The scene's Film
    // must write to outputPfm with resolution width x height.
    bool Start(const std::string &pbrtExe, const std::string &sceneFile,
               const std::string &outputPfm, const std::string &logFile,
               int width, int height, int nThreads);

    // Kill the running render, if any.
    void Stop();

    // Copy the latest RGB image (top-down rows, 3 floats per pixel) if it
    // changed since *version. Returns true if out was written.
    // depth, if not null, gets pbrt camera-space z per pixel (+inf for no
    // hit), or is cleared when pbrt hasn't sent depth.
    // primIds/instanceIds, if not null, get the ids of the nearest hit per
    // pixel (-1 for none, as float values), or are cleared when pbrt hasn't
    // sent them.
    bool Snapshot(std::vector<float> *out, int *width, int *height,
                  uint64_t *version, std::vector<float> *depth = nullptr,
                  std::vector<float> *primIds = nullptr,
                  std::vector<float> *instanceIds = nullptr) const;

    // True once the current pbrt process has exited (and its final image,
    // if any, has been loaded).
    bool IsFinished() const { return _finished.load(); }
    bool Failed() const { return _failed.load(); }
    bool IsRunning() const { return _pid.load() > 0; }

    double ElapsedSeconds() const;

private:
    void _ServerLoop();
    void _HandleMessage(const std::vector<uint8_t> &msg);
    void _Reap(bool block);
    bool _LoadPfm(const std::string &path);

    int _listenFd = -1;
    int _port = 0;
    std::thread _thread;
    std::atomic<bool> _quit{false};

    mutable std::mutex _imageMutex;
    std::vector<float> _image;
    std::vector<float> _depth, _primIds, _instanceIds;
    bool _hasDepth = false, _hasIds = false;
    int _width = 0, _height = 0;
    uint64_t _version = 0;
    bool _hasData = false;  // got pixels from the current process

    std::mutex _procMutex;
    std::atomic<pid_t> _pid{0};
    std::atomic<bool> _finished{true};
    std::atomic<bool> _failed{false};
    std::string _outputPfm, _logFile;
    double _startTime = 0;
};

} // namespace hdpbrt

#endif
