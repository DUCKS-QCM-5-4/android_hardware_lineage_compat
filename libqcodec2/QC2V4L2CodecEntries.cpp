/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android/log.h>
#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <new>

#define LOG_TAG "QC2CodecShim"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// 32-bit ARM little-endian Android libc++ std::string (ABI v1, 12 bytes)
struct LibCppString32 {
    union {
        struct {
            uint8_t size_shifted;
            char data[11];
        } s;
        struct {
            uint32_t cap_flagged;
            uint32_t size;
            char* data;
        } l;
    };

    LibCppString32() { std::memset(this, 0, sizeof(*this)); }

    void assign(const char* str) {
        std::memset(this, 0, sizeof(*this));
        const size_t len = std::strlen(str);
        if (len <= 10) {
            s.size_shifted = static_cast<uint8_t>(len << 1);
            std::memcpy(s.data, str, len + 1);
        } else {
            const uint32_t cap = static_cast<uint32_t>((len + 1 + 15) & ~15u);
            char* buf = static_cast<char*>(::operator new(cap));
            std::memcpy(buf, str, len + 1);
            l.cap_flagged = cap | 1u;
            l.size = static_cast<uint32_t>(len);
            l.data = buf;
        }
    }

    const char* c_str() const {
        return (s.size_shifted & 1u) ? l.data : s.data;
    }
};

struct QC2CodecEntry {
    LibCppString32 name;
    uint32_t kind;
    LibCppString32 inMime;
    LibCppString32 outMime;
    LibCppString32 shortName;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
};

struct LibCppVector32 {
    QC2CodecEntry* begin;
    QC2CodecEntry* end;
    QC2CodecEntry* end_cap;

    size_t size() const { return static_cast<size_t>(end - begin); }
    size_t capacity() const { return static_cast<size_t>(end_cap - begin); }
};

static_assert(sizeof(LibCppString32) == 12, "LibCppString32 must be 12 bytes");
static_assert(sizeof(QC2CodecEntry) == 0x44, "QC2CodecEntry must be 0x44 bytes");

struct ExtraCodecDesc {
    const char* name;
    uint32_t kind;
    const char* inMime;
    const char* outMime;
    const char* shortName;
    uint32_t flags;
};

constexpr ExtraCodecDesc kExtraCodecs[] = {
    {"c2.qti.vp8.decoder", 1, "video/x-vnd.on2.vp8", "video/raw", "vp8D", 2},
    {"c2.qti.vp8.encoder", 2, "video/raw", "video/x-vnd.on2.vp8", "vp8E", 0},
    {"c2.qti.mpeg2.decoder", 1, "video/mpeg2", "video/raw", "mpeg2D", 2},
    {"c2.qti.mpeg2.decoder.secure", 1, "video/mpeg2", "video/raw", "mpeg2DSec", 3},
};

bool hasEntry(const LibCppVector32* vec, const char* name) {
    for (const QC2CodecEntry* it = vec->begin; it != vec->end; ++it) {
        if (std::strcmp(it->name.c_str(), name) == 0) return true;
    }
    return false;
}

void appendEntries(LibCppVector32* vec) {
    size_t toAdd = 0;
    for (const auto& c : kExtraCodecs) {
        if (!hasEntry(vec, c.name)) ++toAdd;
    }
    if (toAdd == 0) return;

    const size_t oldCount = vec->size();
    const size_t newCount = oldCount + toAdd;

    if (newCount > vec->capacity()) {
        const size_t newCap = newCount;
        auto* newBuf = static_cast<QC2CodecEntry*>(::operator new(newCap * sizeof(QC2CodecEntry)));
        if (oldCount > 0 && vec->begin) {
            std::memcpy(newBuf, vec->begin, oldCount * sizeof(QC2CodecEntry));
            ::operator delete(vec->begin);
        }
        vec->begin = newBuf;
        vec->end = newBuf + oldCount;
        vec->end_cap = newBuf + newCap;
    }

    for (const auto& c : kExtraCodecs) {
        if (hasEntry(vec, c.name)) continue;
        QC2CodecEntry* dst = vec->end++;
        std::memset(dst, 0, sizeof(*dst));
        dst->name.assign(c.name);
        dst->kind = c.kind;
        dst->inMime.assign(c.inMime);
        dst->outMime.assign(c.outMime);
        dst->shortName.assign(c.shortName);
        dst->flags = c.flags;
        ALOGI("Injected %s (%s -> %s)", c.name, c.inMime, c.outMime);
    }
}

using FnGetEntries = LibCppVector32* (*)();

__attribute__((constructor))
void qc2_shim_init() {
    void* handle = dlopen("libqcodec2_v4l2codec.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        ALOGE("dlopen(libqcodec2_v4l2codec.so) failed: %s", dlerror());
        return;
    }

    auto getEntries = reinterpret_cast<FnGetEntries>(
        dlsym(handle, "_ZN3qc222GetQC2V4L2CodecEntriesEv"));
    if (!getEntries) {
        ALOGE("Failed to resolve _ZN3qc222GetQC2V4L2CodecEntriesEv: %s", dlerror());
        return;
    }

    LibCppVector32* vec = getEntries();
    if (!vec || !vec->begin || vec->end < vec->begin) {
        ALOGE("Invalid QC2CodecEntry vector pointer: %p", vec);
        return;
    }

    const size_t before = vec->size();
    if (before < 4 || before > 32) {
        ALOGE("Unexpected stock codec count (%zu), aborting injection", before);
        return;
    }

    ALOGI("Stock QC2V4L2CodecEntries count: %zu", before);
    appendEntries(vec);
    ALOGI("Patched QC2V4L2CodecEntries count: %zu", vec->size());
}

}  // namespace