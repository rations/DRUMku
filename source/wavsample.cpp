// WAV loading + resampling. See wavsample.h for the threading contract.

#include "wavsample.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace DRUMku {

namespace {

// ---- little-endian readers over an in-memory buffer, all bounds-checked ----

inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Read the whole file into memory. Returns false on any I/O error or if the
// file is implausibly large (guards against a corrupt/huge header claim).
bool read_file(const std::string &path, std::vector<uint8_t> &buf)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    long size = std::ftell(f);
    if (size <= 0 || size > (long)512 * 1024 * 1024) { // 512 MiB ceiling
        std::fclose(f);
        return false;
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    buf.resize((size_t)size);
    size_t got = std::fread(buf.data(), 1, (size_t)size, f);
    std::fclose(f);
    return got == (size_t)size;
}

// A single PCM/float source sample converted to float in [-1, 1].
inline float sample_to_float(const uint8_t *p, uint16_t bits, uint16_t format)
{
    if (format == 3) { // IEEE float
        if (bits == 32) {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        if (bits == 64) {
            double v;
            std::memcpy(&v, p, 8);
            return (float)v;
        }
        return 0.0f;
    }
    // PCM integer
    switch (bits) {
        case 16: {
            int16_t v = (int16_t)rd_u16(p);
            return (float)v / 32768.0f;
        }
        case 24: {
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (v & 0x800000)
                v |= ~0xFFFFFF; // sign-extend 24 -> 32
            return (float)v / 8388608.0f;
        }
        case 32: {
            int32_t v = (int32_t)rd_u32(p);
            return (float)v / 2147483648.0f;
        }
        case 8: {
            // 8-bit PCM is unsigned (0..255, centered at 128).
            return ((float)p[0] - 128.0f) / 128.0f;
        }
        default:
            return 0.0f;
    }
}

// Parse a RIFF/WAVE buffer into planar float channels (up to 2; extra channels
// are ignored). Fills channels/frames/sampleRate. Returns false on any
// malformed or unsupported input.
bool parse_wav(const std::vector<uint8_t> &buf, DrumSample &out, double &sourceRate)
{
    const uint8_t *p = buf.data();
    size_t n = buf.size();
    if (n < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0)
        return false;

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = nullptr;
    size_t dataSize = 0;

    size_t off = 12;
    while (off + 8 <= n) {
        const uint8_t *id = p + off;
        uint32_t csize = rd_u32(p + off + 4);
        size_t body = off + 8;
        if (body > n)
            break;
        size_t avail = n - body;
        if (csize > avail)
            csize = (uint32_t)avail; // tolerate a truncated final chunk

        if (std::memcmp(id, "fmt ", 4) == 0 && csize >= 16) {
            format = rd_u16(p + body + 0);
            channels = rd_u16(p + body + 2);
            rate = rd_u32(p + body + 4);
            bits = rd_u16(p + body + 14);
            if (format == 0xFFFE && csize >= 40) {
                // WAVE_FORMAT_EXTENSIBLE: the real format is the first 2 bytes
                // of the SubFormat GUID (at offset 24 within the fmt body).
                format = rd_u16(p + body + 24);
            }
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = p + body;
            dataSize = csize;
        }

        off = body + csize;
        if (csize & 1)
            off += 1; // chunks are word-aligned
    }

    if (!data || channels == 0 || rate == 0 || bits == 0)
        return false;
    if (format != 1 && format != 3)
        return false; // only PCM integer / IEEE float
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32 && bits != 64)
        return false;

    const size_t bytesPerSample = bits / 8;
    const size_t frameBytes = bytesPerSample * channels;
    if (frameBytes == 0)
        return false;
    const size_t frames = dataSize / frameBytes;
    if (frames == 0)
        return false;

    const int outCh = channels >= 2 ? 2 : 1;
    out.channels = outCh;
    out.frames = (int)frames;
    out.ch0.assign(frames, 0.0f);
    if (outCh > 1)
        out.ch1.assign(frames, 0.0f);
    else
        out.ch1.clear();

    for (size_t fr = 0; fr < frames; ++fr) {
        const uint8_t *frame = data + fr * frameBytes;
        out.ch0[fr] = sample_to_float(frame, bits, format);
        if (outCh > 1)
            out.ch1[fr] = sample_to_float(frame + bytesPerSample, bits, format);
    }
    sourceRate = (double)rate;
    return true;
}

// ---- windowed-sinc resampler (Blackman window, fixed half-width) ----

constexpr int kSincHalf = 16; // taps = 2 * kSincHalf

inline double sinc(double x)
{
    if (std::fabs(x) < 1e-9)
        return 1.0;
    double px = M_PI * x;
    return std::sin(px) / px;
}

inline double blackman(double t) // t in [-1, 1]
{
    // 0.42 - 0.5 cos + 0.08 cos2, expressed over the [-1,1] window.
    double a = M_PI * (t + 1.0);
    return 0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2.0 * a);
}

void resample_channel(const std::vector<float> &src, std::vector<float> &dst, size_t dstFrames,
                      double ratio /* dst/src */)
{
    // Low-pass cutoff (in source-nyquist units): 1 when upsampling, 1/ratio
    // when downsampling, to suppress aliasing.
    const double cutoff = ratio >= 1.0 ? 1.0 : ratio;
    const double srcN = (double)src.size();
    dst.assign(dstFrames, 0.0f);
    for (size_t i = 0; i < dstFrames; ++i) {
        double center = (double)i / ratio; // position in source samples
        long i0 = (long)std::floor(center) - kSincHalf + 1;
        long i1 = (long)std::floor(center) + kSincHalf;
        double acc = 0.0, norm = 0.0;
        for (long k = i0; k <= i1; ++k) {
            double w = blackman((double)(k - (long)std::floor(center)) / (double)kSincHalf);
            double s = cutoff * sinc(cutoff * (center - (double)k));
            double coeff = w * s;
            norm += coeff;
            if (k >= 0 && (double)k < srcN)
                acc += coeff * (double)src[(size_t)k];
        }
        dst[i] = (float)(norm > 1e-12 ? acc / norm : acc);
    }
}

} // namespace

bool wav_load_resampled(const std::string &path, double targetRate, DrumSample &out)
{
    if (path.empty() || targetRate <= 0.0)
        return false;

    std::vector<uint8_t> buf;
    if (!read_file(path, buf))
        return false;

    DrumSample raw;
    double sourceRate = 0.0;
    if (!parse_wav(buf, raw, sourceRate))
        return false;

    // No conversion needed when the rates already match.
    if (std::fabs(sourceRate - targetRate) < 0.5) {
        out = std::move(raw);
        return true;
    }

    const double ratio = targetRate / sourceRate;
    const size_t dstFrames = (size_t)std::ceil((double)raw.frames * ratio);
    if (dstFrames == 0)
        return false;

    out.channels = raw.channels;
    out.frames = (int)dstFrames;
    resample_channel(raw.ch0, out.ch0, dstFrames, ratio);
    if (raw.channels > 1)
        resample_channel(raw.ch1, out.ch1, dstFrames, ratio);
    else
        out.ch1.clear();
    return true;
}

} // namespace DRUMku
