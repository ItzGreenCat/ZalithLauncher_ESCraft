#include <jni.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/rect.h>
#include <android/dlext.h>
#include <time.h>
#include <string.h>

#include <environ/environ.h>
#include "ctxbridges/egl_loader.h"
#include "ctxbridges/renderer_config.h"
#include "utils.h"
#include "ctxbridges/bridge_tbl.h"

#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
#define GLFW_OPENGL_ES_API 0x30002

#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

EXTERNAL_API EGLConfig config = NULL;
EXTERNAL_API struct PotatoBridge potatoBridge;

// ==========================================================================
// [关键] GLESv2 库句柄
// ==========================================================================
static void* g_GLESv2_Handle = NULL;

// --------------------------------------------------------------------------
// [核心修复] 函数地址获取器
// --------------------------------------------------------------------------
EXTERNAL_API void* pojavGetProcAddress(const char* procname) {
    if (!procname) return NULL;

    // 1. 必须优先从 libGLESv2.so 中寻找核心函数 (如 glGetString)
    // Android 的 eglGetProcAddress 不会返回这些核心函数！
    if (g_GLESv2_Handle) {
        void* addr = dlsym(g_GLESv2_Handle, procname);
        if (addr) return addr;
    }

    // 2. 如果核心库里没有，再尝试 RTLD_DEFAULT (可能在 EGL 里)
    void* addr = dlsym(RTLD_DEFAULT, procname);
    
    // 3. 调试日志 (如果还是崩，开启这个看看到底哪个函数没找到)
    // if (!addr) printf("EGLBridge: Failed to resolve symbol: %s\n", procname);

    return addr;
}

// --------------------------------------------------------------------------
// 初始化
// --------------------------------------------------------------------------
int pojavInitOpenGL() {
    printf("EGLBridge: Force SYSTEM GLES (Hardcoded Path)...\n");

    // [关键] 显式加载 GLESv2 库，并保存句柄
    // 必须用绝对路径，防止加载到 gl4es
    if (!g_GLESv2_Handle) {
        g_GLESv2_Handle = dlopen("/system/lib64/libGLESv2.so", RTLD_LOCAL | RTLD_LAZY);
        if (!g_GLESv2_Handle) {
            g_GLESv2_Handle = dlopen("/system/lib/libGLESv2.so", RTLD_LOCAL | RTLD_LAZY);
        }
        if (!g_GLESv2_Handle) {
             g_GLESv2_Handle = dlopen("/vendor/lib64/libGLESv2.so", RTLD_LOCAL | RTLD_LAZY);
        }
        
        if (g_GLESv2_Handle) {
            printf("EGLBridge: Loaded libGLESv2.so successfully at %p\n", g_GLESv2_Handle);
        } else {
            printf("EGLBridge: [FATAL] Failed to load libGLESv2.so! dlerror: %s\n", dlerror());
            // 如果连 GLESv2 都加载不到，glGetString 必崩
            abort(); 
        }
    }

    // 设置环境
    pojav_environ->config_renderer = RENDERER_GL4ES;
    unsetenv("LIBGL_EGL");
    unsetenv("LIBGL_GLES");
    unsetenv("POJAVEXEC_EGL");

    // 调用 loader (egl_loader.c)
    set_gl_bridge_tbl(); 
    
    if (br_init()) {
        br_setup_window();
    } else {
        printf("EGLBridge: br_init failed!\n");
        return -1;
    }
    return 0;
}

EXTERNAL_API int pojavInit() {
    if (!pojav_environ->pojavWindow) return 0;
    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow, 
                                     pojav_environ->savedWidth, 
                                     pojav_environ->savedHeight, 
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    setenv("VULKAN_PTR", "0", 1); 

    if (pojavInitOpenGL() != 0) return 0;
    return 1;
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) { /* Hooked inside egl_loader */ }
EXTERNAL_API void* pojavCreateContext(void* contextSrc) { return br_init_context((basic_render_window_t*)contextSrc); }
EXTERNAL_API void pojavSwapBuffers() { br_swap_buffers(); }
EXTERNAL_API void pojavMakeCurrent(void* window) { br_make_current((basic_render_window_t*)window); }
EXTERNAL_API void pojavSwapInterval(int interval) { br_swap_interval(interval); }
EXTERNAL_API void pojavTerminate() { }
EXTERNAL_API void* pojavGetCurrentContext() { return br_get_current(); }

JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_setupBridgeWindow(JNIEnv* env, ABI_COMPAT jclass clazz, jobject surface) {
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
    if (br_setup_window) br_setup_window();
}
JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass clazz) {
    if (pojav_environ->pojavWindow) ANativeWindow_release(pojav_environ->pojavWindow);
}
EXTERNAL_API JNIEXPORT jint JNICALL Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(JNIEnv *env, jclass clazz) { return 0; }
EXTERNAL_API JNIEXPORT jlong JNICALL Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) { return 0; }
EXTERNAL_API void* maybe_load_vulkan() { return NULL; }
