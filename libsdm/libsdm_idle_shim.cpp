/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace {

using StrategyCtorFn = void (*)(void* thiz, int32_t display_id, int32_t display_type,
                                void* buffer_allocator, const void* hw_panel_info,
                                const void* hw_resource_info, const void* mixer_attributes,
                                const void* fb_config);
using StrategyDtorFn = void (*)(void* thiz);
using PartialUpdateCreateFn = int (*)(int32_t display_id, int32_t display_type,
                                      const void* hw_resource_info, const void* hw_panel_info,
                                      const void* mixer_attributes, const void* display_attributes,
                                      const void* fb_config, void** partial_update_impl);
using SetIdleTimeoutMsFn = int (*)(void* thiz, uint32_t active_ms, uint32_t inactive_ms);
using DebugGetPropertyFn = int (*)(const char* property_name, int* value);

constexpr int kIdleTimeoutActiveMs = 70;
constexpr int kIdleTimeoutInactiveMs = 520;
constexpr size_t kMaxDisplays = 16;

struct StrategyEntry {
    int32_t display_id;
    int32_t display_type;
    void* strategy;
};

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
StrategyEntry g_strategies[kMaxDisplays] = {};

StrategyCtorFn g_orig_ctor = nullptr;
StrategyDtorFn g_orig_dtor = nullptr;
PartialUpdateCreateFn g_orig_pu_create = nullptr;
SetIdleTimeoutMsFn g_set_idle_fn = nullptr;
DebugGetPropertyFn g_get_prop_fn = nullptr;
bool g_resolved = false;

void resolveSymbolsLocked() {
    if (g_resolved) return;

    void* ext_handle = dlopen("libsdmextension.so", RTLD_NOLOAD);
    if (ext_handle) {
        g_orig_ctor = reinterpret_cast<StrategyCtorFn>(dlsym(
            ext_handle,
            "_ZN3sdm12StrategyImplC2EiNS_11DisplayTypeEPNS_15BufferAllocatorERKNS_11HWPanelInfoERKNS_14HWResourceInfoERKNS_17HWMixerAttributesERKNS_25DisplayConfigVariableInfoE"));
        g_orig_dtor = reinterpret_cast<StrategyDtorFn>(
            dlsym(ext_handle, "_ZN3sdm12StrategyImplD2Ev"));
        g_orig_pu_create = reinterpret_cast<PartialUpdateCreateFn>(dlsym(
            ext_handle,
            "_ZN3sdm17PartialUpdateImpl6CreateEiNS_11DisplayTypeERKNS_14HWResourceInfoERKNS_11HWPanelInfoERKNS_17HWMixerAttributesERKNS_19HWDisplayAttributesERKNS_25DisplayConfigVariableInfoEPPS0_"));
        g_set_idle_fn = reinterpret_cast<SetIdleTimeoutMsFn>(
            dlsym(ext_handle, "_ZN3sdm12StrategyImpl16SetIdleTimeoutMsEjj"));
    }
    g_get_prop_fn = reinterpret_cast<DebugGetPropertyFn>(
        dlsym(RTLD_DEFAULT, "_ZN3sdm5Debug11GetPropertyEPKcPi"));
    g_resolved = (ext_handle != nullptr);
}

void applyIdleTimeout(void* strategy_thiz, SetIdleTimeoutMsFn set_idle_fn,
                      DebugGetPropertyFn get_prop_fn) {
    if (!strategy_thiz || !set_idle_fn) return;

    int active_val = kIdleTimeoutActiveMs;
    int inactive_val = kIdleTimeoutInactiveMs;
    if (get_prop_fn) {
        get_prop_fn("vendor.display.idle_time", &active_val);
        get_prop_fn("vendor.display.idle_time_inactive", &inactive_val);
    }
    set_idle_fn(strategy_thiz, static_cast<uint32_t>(active_val),
                static_cast<uint32_t>(inactive_val));
}

}  // namespace

extern "C" {

__attribute__((visibility("default")))
void _ZN3sdm12StrategyImplC2EiNS_11DisplayTypeEPNS_15BufferAllocatorERKNS_11HWPanelInfoERKNS_14HWResourceInfoERKNS_17HWMixerAttributesERKNS_25DisplayConfigVariableInfoE(
    void* thiz, int32_t display_id, int32_t display_type, void* buffer_allocator,
    const void* hw_panel_info, const void* hw_resource_info, const void* mixer_attributes,
    const void* fb_config) {
    pthread_mutex_lock(&g_mutex);
    resolveSymbolsLocked();
    StrategyCtorFn orig_ctor = g_orig_ctor;
    pthread_mutex_unlock(&g_mutex);

    if (orig_ctor) {
        orig_ctor(thiz, display_id, display_type, buffer_allocator, hw_panel_info,
                  hw_resource_info, mixer_attributes, fb_config);
    }

    if (thiz) {
        pthread_mutex_lock(&g_mutex);
        size_t free_idx = kMaxDisplays;
        for (size_t i = 0; i < kMaxDisplays; ++i) {
            if (g_strategies[i].strategy == thiz ||
                (g_strategies[i].strategy != nullptr &&
                 g_strategies[i].display_id == display_id &&
                 g_strategies[i].display_type == display_type)) {
                free_idx = i;
                break;
            }
            if (g_strategies[i].strategy == nullptr && free_idx == kMaxDisplays) {
                free_idx = i;
            }
        }
        if (free_idx < kMaxDisplays) {
            g_strategies[free_idx] = {display_id, display_type, thiz};
        }
        pthread_mutex_unlock(&g_mutex);
    }
}

__attribute__((visibility("default")))
void _ZN3sdm12StrategyImplD2Ev(void* thiz) {
    pthread_mutex_lock(&g_mutex);
    resolveSymbolsLocked();
    StrategyDtorFn orig_dtor = g_orig_dtor;
    for (size_t i = 0; i < kMaxDisplays; ++i) {
        if (g_strategies[i].strategy == thiz) {
            g_strategies[i].strategy = nullptr;
        }
    }
    pthread_mutex_unlock(&g_mutex);

    if (orig_dtor) {
        orig_dtor(thiz);
    }
}

__attribute__((visibility("default")))
int _ZN3sdm17PartialUpdateImpl6CreateEiNS_11DisplayTypeERKNS_14HWResourceInfoERKNS_11HWPanelInfoERKNS_17HWMixerAttributesERKNS_19HWDisplayAttributesERKNS_25DisplayConfigVariableInfoEPPS0_(
    int32_t display_id, int32_t display_type, const void* hw_resource_info,
    const void* hw_panel_info, const void* mixer_attributes, const void* display_attributes,
    const void* fb_config, void** partial_update_impl) {
    pthread_mutex_lock(&g_mutex);
    resolveSymbolsLocked();
    PartialUpdateCreateFn orig_pu_create = g_orig_pu_create;
    SetIdleTimeoutMsFn set_idle_fn = g_set_idle_fn;
    DebugGetPropertyFn get_prop_fn = g_get_prop_fn;
    void* strategy_thiz = nullptr;
    for (size_t i = 0; i < kMaxDisplays; ++i) {
        if (g_strategies[i].strategy != nullptr &&
            g_strategies[i].display_id == display_id &&
            g_strategies[i].display_type == display_type) {
            strategy_thiz = g_strategies[i].strategy;
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex);

    int ret = orig_pu_create
                  ? orig_pu_create(display_id, display_type, hw_resource_info, hw_panel_info,
                                   mixer_attributes, display_attributes, fb_config,
                                   partial_update_impl)
                  : -1;

    applyIdleTimeout(strategy_thiz, set_idle_fn, get_prop_fn);
    return ret;
}

}  // extern "C"
