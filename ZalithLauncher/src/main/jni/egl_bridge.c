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

// --------------------------------------------------------------------------
// [必须] 函数地址获取器
// --------------------------------------------------------------------------
EXTERNAL_API void* pojavGetProcAddress(const char* procname) {
    // 1. 尝试 dlsym (从全局或者系统库)
    void* addr = dlsym(RTLD_DEFAULT, procname);
    
    // 2. 如果全局找不到，强制去 libGLESv2 找
    if (!addr) {
        static void* sys_gles = NULL;
        if (!sys_gles) sys_gles = dlopen("libGLESv2.so", RTLD_LAZY);
        if (sys_gles) addr = dlsym(sys_gles, procname);
    }
    return addr;
}

// --------------------------------------------------------------------------
// 初始化：无视一切，只走一条路
// --------------------------------------------------------------------------
int pojavInitOpenGL() {
    printf("EGLBridge: Force-Loading System GLES...\n");

    // 欺骗 Pojav 认为它是 GL4ES 模式
    // 这样它就会调用 set_gl_bridge_tbl -> egl_loader.c
    pojav_environ->config_renderer = RENDERER_GL4ES;

    // 清除可能导致干扰的变量
    unsetenv("LIBGL_EGL");
    unsetenv("LIBGL_GLES");
    
    // 加载函数 (这里会触发我们改写过的 egl_loader.c)
    set_gl_bridge_tbl();
    
    // 初始化 EGL
    if (br_init()) {
        br_setup_window();
    } else {
        printf("EGLBridge: [FATAL] br_init failed.\n");
        return -1;
    }
    
    return 0;
}

EXTERNAL_API int pojavInit() {
    if (!pojav_environ->pojavWindow) return 0;
    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    
    // 强制 RGBA 8888
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow, 
                                     pojav_environ->savedWidth, 
                                     pojav_environ->savedHeight, 
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    
    // 禁用 Vulkan
    setenv("VULKAN_PTR", "0", 1); 

    if (pojavInitOpenGL() != 0) return 0;
    return 1;
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;
    // 不做任何事，也不崩溃，全部交给 egl_loader 里的 Hook 处理
}

// 标准接口直接透传
EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    return br_init_context((basic_render_window_t*)contextSrc);
}
EXTERNAL_API void pojavSwapBuffers() { br_swap_buffers(); }
EXTERNAL_API void pojavMakeCurrent(void* window) { br_make_current((basic_render_window_t*)window); }
EXTERNAL_API void pojavSwapInterval(int interval) { br_swap_interval(interval); }
EXTERNAL_API void pojavTerminate() { /* cleanup */ }
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
