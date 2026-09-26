/*
 * Shim for vendor/lib64/libsdmextension.so (libdisplayqos.so).
 * Intercepts QOSParserImpl::DetermineXML and overrides the XML path
 * for specific known HW versions. Any other version falls through
 * to the original implementation unchanged.
 */

#include <android/log.h>
#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#define LOG_TAG "QOSParserImpl_shim"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * Mangled symbol for QOSParserImpl::DetermineXML(int, std::string*)
 * llvm-nm -D --defined-only libdisplayqos.so | grep 'DetermineXML'
 */
#define QOS_DETERMINE_XML_SYMBOL \
    "_ZN13QOSParserImpl12DetermineXMLEiPNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE"

constexpr int kDpuVersionMsmnile = 0x50000000;
constexpr int kDpuVersionMsmnileV1 = 0x50000001;

/* Declaration: binds this name to the real mangled symbol. */
extern "C" __attribute__((visibility("default")))
int DetermineXML_shim(void* thiz, int hw_version, std::string* xml_path)
    __asm__(QOS_DETERMINE_XML_SYMBOL);

int DetermineXML_shim(void* instance, int hw_version, std::string* xml_path) {
    switch (hw_version) {
        case kDpuVersionMsmnile:
        case kDpuVersionMsmnileV1:
            *xml_path = "/vendor/etc/display/DPU5__.xml";
            ALOGI("Redirected hw_version 0x%x to %s", hw_version, xml_path->c_str());
            return 0;
        default:
            break;
    }

    /* No override for this version: resolve and call the original. */
    void* resolved = dlsym(RTLD_NEXT, QOS_DETERMINE_XML_SYMBOL);
    if (!resolved) {
        void* handle = dlopen("libdisplayqos.so", RTLD_NOLOAD);
        if (handle) {
            resolved = dlsym(handle, QOS_DETERMINE_XML_SYMBOL);
        }
    }
    if (!resolved) {
        ALOGE("dlsym failed to find original DetermineXML: %s", dlerror());
        abort();
    }

    ALOGI("Shimmed DetermineXML has no override for hw version %x; "
          "this library likely should not be built for this hw version",
          hw_version);

    using orig_fn_t = int (*)(void*, int, std::string*);
    auto orig_fn = reinterpret_cast<orig_fn_t>(resolved);
    return orig_fn(instance, hw_version, xml_path);
}
