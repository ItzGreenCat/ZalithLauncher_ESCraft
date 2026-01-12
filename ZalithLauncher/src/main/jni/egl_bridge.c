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

static void* g_GLESv2_Handle = NULL;

// --------------------------------------------------------------------------
// [调试] 函数地址获取器
// --------------------------------------------------------------------------
EXTERNAL_API void* pojavGetProcAddress(const char* procname) {
    if (!procname) return NULL;
    
    // [DEBUG] 打印 LWJGL 请求的函数名
    // printf("EGLBridge: Requesting symbol: %s\n", procname);

    void* addr = NULL;

    // 1. 优先从显式加载的 GLESv2 句柄找
    if (g_GLESv2_Handle) {
        addr = dlsym(g_GLESv2_Handle, procname);
    }

    // 2. 如果没找到，尝试全局查找
    if (!addr) {
        addr = dlsym(RTLD_DEFAULT, procname);
    }
    
    // [DEBUG] 如果找到了核心函数，打印出来确认
    if (addr && strcmp(procname, "glGetString") == 0) {
        printf("EGLBridge: FOUND glGetString at %p\n", addr);
    }

    return addr;
}

// --------------------------------------------------------------------------
// 初始化
// --------------------------------------------------------------------------
int pojavInitOpenGL() {
    printf("EGLBridge: Force SYSTEM GLES (Global + Filename Mode)...\n");

    // [关键修复] 使用 RTLD_GLOBAL | RTLD_LAZY
    // 这会将符号暴露给全局，极大增加 LWJGL 找到它们的概率
    int flags = RTLD_GLOBAL | RTLD_LAZY;

    if (!g_GLESv2_Handle) {
        // 使用文件名加载，符合 NDK 规范
        g_GLESv2_Handle = dlopen("libGLESv2.so", flags);
        
        // 绝对路径回退
        if (!g_GLESv2_Handle) g_GLESv2_Handle = dlopen("/system/lib64/libGLESv2.so", flags);
        if (!g_GLESv2_Handle) g_GLESv2_Handle = dlopen("/system/lib/libGLESv2.so", flags);
        
        if (g_GLESv2_Handle) {
            printf("EGLBridge: Loaded 'libGLESv2.so' at %p (GLOBAL)\n", g_GLESv2_Handle);
        } else {
            printf("EGLBridge: [FATAL] Failed to load 'libGLESv2.so'!\n");
            abort(); 
        }
    }

    pojav_environ->config_renderer = RENDERER_GL4ES;
    unsetenv("LIBGL_EGL");
    unsetenv("LIBGL_GLES");
    unsetenv("POJAVEXEC_EGL");

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

// [DEBUG] 增加 MakeCurrent 的日志，确保上下文切换成功
EXTERNAL_API void pojavMakeCurrent(void* window) { 
    // printf("EGLBridge: pojavMakeCurrent called.\n");
    br_make_current((basic_render_window_t*)window); 
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) { }
EXTERNAL_API void* pojavCreateContext(void* contextSrc) { return br_init_context((basic_render_window_t*)contextSrc); }
EXTERNAL_API void pojavSwapBuffers() { br_swap_buffers(); }
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
