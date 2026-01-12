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

// 那些 VirGL 的引用可以删掉或者留着，反正我们不会去调用它们
#include "ctxbridges/virgl_bridge.h" 
#include "ctxbridges/osm_bridge.h"

#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
#define GLFW_OPENGL_ES_API 0x30002

#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

EXTERNAL_API EGLConfig config = NULL;
EXTERNAL_API struct PotatoBridge potatoBridge;

// --------------------------------------------------------------------------
// [必须] LWJGL 函数获取器 (硬编码系统路径)
// --------------------------------------------------------------------------
EXTERNAL_API void* pojavGetProcAddress(const char* procname) {
    // 1. 尝试默认查找
    void* addr = dlsym(RTLD_DEFAULT, procname);
    
    // 2. 强制去系统 GLESv2 库找 (绝对路径，防止找到 gl4es)
    if (!addr) {
        static void* sys_gles = NULL;
        // 懒加载系统库句柄
        if (!sys_gles) sys_gles = dlopen("/system/lib64/libGLESv2.so", RTLD_LAZY);
        if (!sys_gles) sys_gles = dlopen("/system/lib/libGLESv2.so", RTLD_LAZY);
        
        if (sys_gles) addr = dlsym(sys_gles, procname);
    }
    return addr;
}

// --------------------------------------------------------------------------
// 初始化：无视选项，强制加载
// --------------------------------------------------------------------------
int pojavInitOpenGL() {
    printf("EGLBridge: Ignoring POJAV_RENDERER. Forcing SYSTEM GLES.\n");

    // 设置逻辑状态为 GL4ES，因为我们要复用它的 br_init 代码路径
    // 但因为我们在 egl_loader.c 里硬编码了加载逻辑，所以实际加载的是 System EGL
    pojav_environ->config_renderer = RENDERER_GL4ES;

    // 清除环境变量，防止干扰
    unsetenv("LIBGL_EGL");
    unsetenv("LIBGL_GLES");
    unsetenv("POJAVEXEC_EGL");
    unsetenv("GALLIUM_DRIVER"); 

    // 调用 egl_loader.c (它现在只加载 /system/lib64/libEGL.so)
    set_gl_bridge_tbl(); 
    
    // 使用 EGL 创建 Display 和 Context
    if (br_init()) {
        br_setup_window();
    } else {
        printf("EGLBridge: [FATAL] br_init (EGL setup) failed!\n");
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
    
    setenv("VULKAN_PTR", "0", 1); 

    if (pojavInitOpenGL() != 0) return 0;
    return 1;
}

// --------------------------------------------------------------------------
// 杂项接口
// --------------------------------------------------------------------------
EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    // 允许任何 API 请求，我们的 Hook 会处理
}

EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    // 永远只走 br_init_context (EGL)
    return br_init_context((basic_render_window_t*)contextSrc);
}

EXTERNAL_API void pojavSwapBuffers() { br_swap_buffers(); }
EXTERNAL_API void pojavMakeCurrent(void* window) { br_make_current((basic_render_window_t*)window); }
EXTERNAL_API void pojavSwapInterval(int interval) { br_swap_interval(interval); }
EXTERNAL_API void* pojavGetCurrentContext() { return br_get_current(); }
EXTERNAL_API void pojavTerminate() { /* cleanup */ }

// JNI
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
