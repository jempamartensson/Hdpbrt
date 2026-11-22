// Simple CPU render buffer (adapted from USD's hdEmbree, Apache-2.0).
#ifndef HDPBRT_RENDER_BUFFER_H
#define HDPBRT_RENDER_BUFFER_H

#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/base/gf/vec3i.h"
#include <algorithm>
#include <atomic>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdPbrtRenderBuffer : public HdRenderBuffer
{
public:
    HdPbrtRenderBuffer(SdfPath const &id);
    ~HdPbrtRenderBuffer() override;

    bool Allocate(GfVec3i const &dimensions, HdFormat format,
                  bool multiSampled) override;
    unsigned int GetWidth() const override { return _width; }
    unsigned int GetHeight() const override { return _height; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return false; }
    void *Map() override { _mappers++; return _buffer.data(); }
    void Unmap() override { _mappers--; }
    bool IsMapped() const override { return _mappers.load() != 0; }
    bool IsConverged() const override { return _converged.load(); }
    void SetConverged(bool cv) { _converged.store(cv); }
    void Resolve() override {}

    // Write a top-down RGB float image of size w x h into the data window
    // starting at (x0, y0) (y-down), converting to the buffer format.
    void WriteRGB(const float *rgb, int w, int h, int x0, int y0);
    void Clear(const float *value, size_t numComponents);

private:
    void _Deallocate() override;

    unsigned int _width = 0, _height = 0;
    HdFormat _format = HdFormatInvalid;
    std::vector<uint8_t> _buffer;
    std::atomic<int> _mappers{0};
    std::atomic<bool> _converged{false};
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
