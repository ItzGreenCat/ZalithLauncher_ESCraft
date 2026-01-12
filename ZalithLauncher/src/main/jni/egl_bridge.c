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
#include <GL/osmesa.h>
#include "ctxbridges/egl_loader.h"
#include "ctxbridges/osmesa_loader.h"
#include "ctxbridges/renderer_config.h"
#include "ctxbridges/virgl_bridge.h"
#include "driver_helper/nsbypass.h"

#ifdef GLES_TEST
#include <GLES2/gl2.h>
#endif

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/rect.h>
#include <string.h>

// [FIX] 必须包含此头文件
#include <environ/environ.h>

#include <android/dlext.h>
#include <time.h>
#include "utils.h"
#include "ctxbridges/bridge_tbl.h"
#include "ctxbridges/osm_bridge.h"

#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
// [ADD] 新增 GLES API 定义
#define GLFW_OPENGL_ES_API 0x30002

// [FIX] 增加 visibility("default") 解决 undefined symbol 链接错误
#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

// [FIX] 显式导出全局变量
EXTERNAL_API EGLConfig config;
EXTERNAL_API struct PotatoBridge potatoBridge;

void* loadTurnipVulkan();
void calculateFPS();

// --------------------------------------------------------------------------
// [ADD] 核心补丁：函数地址获取器
// 如果没有这个函数，LWJGL 无法获取 GLES 函数指针，会报 "No context current"
// --------------------------------------------------------------------------
EXTERNAL_API void* pojavGetProcAddress(const char* procname) {
    // 简单直接：去系统库里找
    void* addr = dlsym(RTLD_DEFAULT, procname);
    if (!addr) {
        // 保底尝试加载 GLESv2
        static void* sys_gles = NULL;
        if (!sys_gles) sys_gles = dlopen("libGLESv2.so", RTLD_LAZY);
        if (sys_gles) addr = dlsym(sys_gles, procname);
    }
    return addr;
}

EXTERNAL_API void pojavTerminate() {
    printf("EGLBridge: Terminating\n");

    switch (pojav_environ->config_renderer) {
        case RENDERER_GL4ES: {
            eglMakeCurrent_p(potatoBridge.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface_p(potatoBridge.eglDisplay, potatoBridge.eglSurface);
            eglDestroyContext_p(potatoBridge.eglDisplay, potatoBridge.eglContext);
            eglTerminate_p(potatoBridge.eglDisplay);
            eglReleaseThread_p();

            potatoBridge.eglContext = EGL_NO_CONTEXT;
            potatoBridge.eglDisplay = EGL_NO_DISPLAY;
            potatoBridge.eglSurface = EGL_NO_SURFACE;
        } break;
        case RENDERER_VK_ZINK: {
            // Nothing to do here
        } break;
    }
}

JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_setupBridgeWindow(JNIEnv* env, ABI_COMPAT jclass clazz, jobject surface) {
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
    if (br_setup_window) br_setup_window();
}

JNIEXPORT void JNICALL
Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass clazz) {
    if (pojav_environ->pojavWindow) ANativeWindow_release(pojav_environ->pojavWindow);
}

EXTERNAL_API void* pojavGetCurrentContext() {
    if (pojav_environ->config_renderer == RENDERER_VIRGL)
        return virglGetCurrentContext();

    return br_get_current();
}

static void set_vulkan_ptr(void* ptr) {
    char envval[64];
    sprintf(envval, "%"PRIxPTR, (uintptr_t)ptr);
    setenv("VULKAN_PTR", envval, 1);
}

void load_vulkan() {
    // 省略复杂逻辑，防止干扰
    if(getenv("VULKAN_PTR") == NULL) {
         void* vPtr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
         if (vPtr) set_vulkan_ptr(vPtr);
    }
}

// --------------------------------------------------------------------------
// [MOD] 初始化逻辑：强制 System GLES
// --------------------------------------------------------------------------
int pojavInitOpenGL() {
    const char *forceVsync = getenv("FORCE_VSYNC");
    if (forceVsync && !strcmp(forceVsync, "true"))
        pojav_environ->force_vsync = true;

    // [MOD] 1. 强制设置环境变量，欺骗 egl_loader 加载系统库
    // POJAVEXEC_EGL 是 egl_loader.c 优先检查的变量
    setenv("POJAVEXEC_EGL", "libEGL.so", 1);
    setenv("LIBGL_EGL", "libEGL.so", 1);
    setenv("LIBGL_GLES", "libGLESv2.so", 1);
    
    printf("EGLBridge: Forced System GLES (Redirected to libEGL.so)\n");

    load_vulkan();

    // [MOD] 2. 总是走 GL4ES 路径 (但实际上加载的是系统库)
    pojav_environ->config_renderer = RENDERER_GL4ES;
    
    // 这会调用 egl_loader.c 的加载逻辑，因为上面设了环境变量，它会加载系统库
    set_gl_bridge_tbl();

    // [MOD] 3. 强制绑定 API (防止 EGL 默认 Desktop GL)
    // dlsym_EGL 执行完后，eglBindAPI_p 指针应该已经指向系统函数了
    if (eglBindAPI_p) {
        printf("EGLBridge: Binding API to GLES (0x30A0)...\n");
        eglBindAPI_p(0x30A0); // EGL_OPENGL_ES_API
    }

    if (br_init()) br_setup_window();

    return 0;
}

EXTERNAL_API int pojavInit() {
    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    
    // [MOD] 强制 RGBA 8888 格式
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow,
                                     pojav_environ->savedWidth,
                                     pojav_environ->savedHeight,
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    pojavInitOpenGL();
    return 1;
}

// --------------------------------------------------------------------------
// [MOD] Hint 处理：允许 GLES
// --------------------------------------------------------------------------
EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;
    switch (value) {
        case GLFW_NO_API:
            pojav_environ->config_renderer = RENDERER_VULKAN;
            break;
        case GLFW_OPENGL_API:
            break;
        case GLFW_OPENGL_ES_API:
            // [MOD] 允许 ES 请求，不要 abort
            printf("EGLBridge: Accepted GLFW_OPENGL_ES_API hint.\n");
            break;
        default:
            printf("GLFW: Unimplemented API 0x%x\n", value);
            // abort(); // [MOD] 为了稳健，注释掉崩溃
    }
}

EXTERNAL_API void pojavSwapBuffers() {
    calculateFPS();
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK
     || pojav_environ->config_renderer == RENDERER_GL4ES)
    {
        br_swap_buffers();
    }
    else if (pojav_environ->config_renderer == RENDERER_VIRGL)
    {
        virglSwapBuffers();
    }
}

EXTERNAL_API void pojavMakeCurrent(void* window) {
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK
     || pojav_environ->config_renderer == RENDERER_GL4ES)
    {
        br_make_current((basic_render_window_t*)window);
    }
    else if (pojav_environ->config_renderer == RENDERER_VIRGL)
    {
        virglMakeCurrent(window);
    }
}

EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    if (pojav_environ->config_renderer == RENDERER_VULKAN)
        return (void *) pojav_environ->pojavWindow;
    if (pojav_environ->config_renderer == RENDERER_VIRGL)
        return virglCreateContext(contextSrc);

    return br_init_context((basic_render_window_t*)contextSrc);
}

// [FIX] 加上 EXTERNAL_API 解决 linker error
EXTERNAL_API void* maybe_load_vulkan() {
    if(getenv("VULKAN_PTR") == NULL) load_vulkan();
    // 防止 strtoul 处理 NULL 崩溃
    const char* ptr = getenv("VULKAN_PTR");
    return ptr ? (void*) strtoul(ptr, NULL, 0x10) : NULL;
}

static int frameCount = 0;
static int fps = 0;
static time_t lastTime = 0;

void calculateFPS() {
    frameCount++;
    time_t currentTime = time(NULL);

    if (currentTime != lastTime) {
        lastTime = currentTime;
        fps = frameCount;
        frameCount = 0;
    }
}

EXTERNAL_API JNIEXPORT jint JNICALL
Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(JNIEnv *env, jclass clazz) {
    return fps;
}

EXTERNAL_API JNIEXPORT jlong JNICALL
Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    printf("EGLBridge: LWJGL-side Vulkan loader requested the Vulkan handle\n");
    return (jlong) maybe_load_vulkan();
}

EXTERNAL_API void pojavSwapInterval(int interval) {
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK
     || pojav_environ->config_renderer == RENDERER_GL4ES)
    {
        br_swap_interval(interval);
    }
    else if (pojav_environ->config_renderer == RENDERER_VIRGL)
    {
        virglSwapInterval(interval);
    }
}
