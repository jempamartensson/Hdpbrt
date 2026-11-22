#include "renderBuffer.h"
#include "pxr/base/gf/half.h"
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

HdPbrtRenderBuffer::HdPbrtRenderBuffer(SdfPath const &id) : HdRenderBuffer(id) {}
HdPbrtRenderBuffer::~HdPbrtRenderBuffer() = default;

void HdPbrtRenderBuffer::_Deallocate()
{
    _width = _height = 0;
    _format = HdFormatInvalid;
    _buffer.clear();
    _mappers.store(0);
    _converged.store(false);
}

bool HdPbrtRenderBuffer::Allocate(GfVec3i const &dims, HdFormat format, bool)
{
    _Deallocate();
    if (dims[2] != 1)
        return false;
    _width = dims[0];
    _height = dims[1];
    _format = format;
    _buffer.assign(size_t(_width) * _height * HdDataSizeOfFormat(format), 0);
    return true;
}

static void _WritePixel(HdFormat format, uint8_t *dst, size_t n, const float *v)
{
    HdFormat cf = HdGetComponentFormat(format);
    size_t cc = HdGetComponentCount(format);
    for (size_t c = 0; c < cc; ++c) {
        float x = c < n ? v[c] : (c == 3 ? 1.f : 0.f);
        switch (cf) {
        case HdFormatFloat32: ((float *)dst)[c] = x; break;
        case HdFormatFloat16: ((uint16_t *)dst)[c] = GfHalf(x).bits(); break;
        case HdFormatUNorm8:
            ((uint8_t *)dst)[c] = (uint8_t)(std::min(std::max(x, 0.f), 1.f) * 255.f + .5f);
            break;
        case HdFormatSNorm8:
            ((int8_t *)dst)[c] = (int8_t)(std::min(std::max(x, -1.f), 1.f) * 127.f);
            break;
        case HdFormatInt32: ((int32_t *)dst)[c] = (int32_t)x; break;
        default: break;
        }
    }
}

void HdPbrtRenderBuffer::Clear(const float *value, size_t n)
{
    size_t fs = HdDataSizeOfFormat(_format);
    if (!fs)
        return;
    for (size_t i = 0; i < size_t(_width) * _height; ++i)
        _WritePixel(_format, &_buffer[i * fs], n, value);
}

void HdPbrtRenderBuffer::WriteRGB(const float *rgb, int w, int h, int x0, int y0)
{
    size_t fs = HdDataSizeOfFormat(_format);
    if (!fs)
        return;
    const size_t cc = HdGetComponentCount(_format);
    for (int y = 0; y < h; ++y) {
        int yy = y0 + y;  // y-down
        if (yy < 0 || yy >= (int)_height)
            continue;
        int row = _height - 1 - yy;  // buffer rows go bottom to top
        for (int x = 0; x < w; ++x) {
            int xx = x0 + x;
            if (xx < 0 || xx >= (int)_width)
                continue;
            const float *s = rgb + (size_t(y) * w + x) * 3;
            float v[4] = {s[0], s[1], s[2], 1.f};
            if (cc == 1)  // single channel: luminance
                v[0] = 0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2];
            _WritePixel(_format, &_buffer[(size_t(row) * _width + xx) * fs], 4, v);
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
