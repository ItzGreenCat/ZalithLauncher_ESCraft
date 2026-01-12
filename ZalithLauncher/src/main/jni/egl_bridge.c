#include <jni.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/rect.h>
#include <android/dlext.h>
#include <time.h>

#include <environ/environ.h>
#include <EGL/egl.h>

// 引入这些头文件是为了让 struct PotatoBridge 的定义可见
#include "ctxbridges/egl_loader.h"
#include "ctxbridges/renderer_config.h"

// -------------------------------------------------------------
// Macros & Config
// -------------------------------------------------------------
#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
#define GLFW_OPENGL_ES_API 0x30002

// [CRITICAL] 强制导出符号，解决 "undefined symbol" 链接错误
#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

#ifndef RENDERER_GL4ES
#define RENDERER_GL4ES 0
#endif

// -------------------------------------------------------------
// [FIX 1] 恢复全局变量定义 (满足 virgl_bridge.c 的链接需求)
// -------------------------------------------------------------
EXTERNAL_API EGLConfig config = NULL;
EXTERNAL_API struct PotatoBridge potatoBridge;

// -------------------------------------------------------------
// [FIX 2] Vulkan 空实现 (满足 lwjgl_dlopen_hook.c 的链接需求)
// -------------------------------------------------------------
EXTERNAL_API void* maybe_load_vulkan() {
    // 彻底禁用 Vulkan，直接返回 NULL
    return NULL;
}

// 保留 JNI 接口但置空
EXTERNAL_API JNIEXPORT jlong JNICALL Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    return 0;
}

// -------------------------------------------------------------
// 自定义 EGL 函数指针
// -------------------------------------------------------------
static void* lib_egl_handle = NULL;

static EGLDisplay (*sys_eglGetDisplay)(EGLNativeDisplayType) = NULL;
static EGLBoolean (*sys_eglInitialize)(EGLDisplay, EGLint*, EGLint*) = NULL;
static EGLBoolean (*sys_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = NULL;
static EGLContext (*sys_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = NULL;
static EGLSurface (*sys_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = NULL;
static EGLBoolean (*sys_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = NULL;
static EGLBoolean (*sys_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;
static EGLBoolean (*sys_eglSwapInterval)(EGLDisplay, EGLint) = NULL;
static EGLBoolean (*sys_eglBindAPI)(EGLenum) = NULL;
static EGLint     (*sys_eglGetError)(void) = NULL;
static EGLBoolean (*sys_eglDestroySurface)(EGLDisplay, EGLSurface) = NULL;
static EGLBoolean (*sys_eglDestroyContext)(EGLDisplay, EGLContext) = NULL;
static EGLBoolean (*sys_eglTerminate)(EGLDisplay) = NULL;

// 内部使用的全局状态
static EGLDisplay g_Display = EGL_NO_DISPLAY;
static EGLSurface g_Surface = EGL_NO_SURFACE;
static EGLContext g_Context = EGL_NO_CONTEXT;

// -------------------------------------------------------------
// 加载系统 EGL 驱动
// -------------------------------------------------------------
static int load_system_egl_functions() {
    printf("EGLBridge: Loading System EGL library (No Vulkan)...\n");
    
    lib_egl_handle = dlopen("libEGL.so", RTLD_LAZY);
    if (!lib_egl_handle) lib_egl_handle = dlopen("/system/lib64/libEGL.so", RTLD_LAZY);
    if (!lib_egl_handle) lib_egl_handle = dlopen("/system/lib/libEGL.so", RTLD_LAZY);
    
    if (!lib_egl_handle) {
        printf("EGLBridge: [FATAL] Could not dlopen libEGL.so! Error: %s\n", dlerror());
        return 0;
    }

    sys_eglGetDisplay = dlsym(lib_egl_handle, "eglGetDisplay");
    sys_eglInitialize = dlsym(lib_egl_handle, "eglInitialize");
    sys_eglChooseConfig = dlsym(lib_egl_handle, "eglChooseConfig");
    sys_eglCreateContext = dlsym(lib_egl_handle, "eglCreateContext");
    sys_eglCreateWindowSurface = dlsym(lib_egl_handle, "eglCreateWindowSurface");
    sys_eglMakeCurrent = dlsym(lib_egl_handle, "eglMakeCurrent");
    sys_eglSwapBuffers = dlsym(lib_egl_handle, "eglSwapBuffers");
    sys_eglSwapInterval = dlsym(lib_egl_handle, "eglSwapInterval");
    sys_eglBindAPI = dlsym(lib_egl_handle, "eglBindAPI");
    sys_eglGetError = dlsym(lib_egl_handle, "eglGetError");
    sys_eglDestroySurface = dlsym(lib_egl_handle, "eglDestroySurface");
    sys_eglDestroyContext = dlsym(lib_egl_handle, "eglDestroyContext");
    sys_eglTerminate = dlsym(lib_egl_handle, "eglTerminate");

    if (!sys_eglGetDisplay || !sys_eglCreateContext) {
        printf("EGLBridge: [FATAL] Missing EGL symbols!\n");
        return 0;
    }
    return 1;
}

// -------------------------------------------------------------
// 初始化 EGL
// -------------------------------------------------------------
int pojavInitOpenGL() {
    if (!load_system_egl_functions()) return -1;

    g_Display = sys_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_Display == EGL_NO_DISPLAY) return -1;

    if (!sys_eglInitialize(g_Display, NULL, NULL)) return -1;

    if (sys_eglBindAPI) sys_eglBindAPI(0x30A0); // EGL_OPENGL_ES_API

    printf("EGLBridge: EGL Initialized (Display: %p)\n", g_Display);
    
    // 设置状态，同时填充 potatoBridge 以防万一
    pojav_environ->config_renderer = RENDERER_GL4ES; 
    potatoBridge.eglDisplay = g_Display;

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
    if (pojavInitOpenGL() != 0) return 0; 
    return 1; 
}

// -------------------------------------------------------------
// 创建 Context
// -------------------------------------------------------------
EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    (void)contextSrc;
    if (g_Display == EGL_NO_DISPLAY) return NULL;

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, 0x0004, // ES2 bit
        EGL_BLUE_SIZE,       8,
        EGL_GREEN_SIZE,      8,
        EGL_RED_SIZE,        8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      16,     // 16-bit Depth
        EGL_STENCIL_SIZE,    0,
        EGL_NONE
    };

    EGLint num_configs;
    if (!sys_eglChooseConfig(g_Display, attribs, &config, 1, &num_configs) || num_configs == 0) {
        printf("EGLBridge: [FATAL] No suitable EGL Config found!\n");
        return NULL;
    }

    const EGLint ctx_attribs[] = { 0x3098, 3, EGL_NONE }; // GLES 3.0
    g_Context = sys_eglCreateContext(g_Display, config, EGL_NO_CONTEXT, ctx_attribs);
    
    if (g_Context == EGL_NO_CONTEXT) {
        const EGLint ctx_attribs_2[] = { 0x3098, 2, EGL_NONE };
        g_Context = sys_eglCreateContext(g_Display, config, EGL_NO_CONTEXT, ctx_attribs_2);
    }

    if (g_Context == EGL_NO_CONTEXT) return NULL;

    g_Surface = sys_eglCreateWindowSurface(g_Display, config, (EGLNativeWindowType)pojav_environ->pojavWindow, NULL);
    if (g_Surface == EGL_NO_SURFACE) return NULL;

    sys_eglMakeCurrent(g_Display, g_Surface, g_Surface, g_Context);

    // 填充 potatoBridge
    potatoBridge.eglContext = g_Context;
    potatoBridge.eglSurface = g_Surface;

    return (void*)g_Context; 
}

// -------------------------------------------------------------
// 其他接口
// -------------------------------------------------------------
EXTERNAL_API void pojavSwapBuffers() {
    static int frameCount = 0;
    static time_t lastTime = 0;
    frameCount++;
    time_t currentTime = time(NULL);
    if (currentTime != lastTime) { lastTime = currentTime; frameCount = 0; }

    if (sys_eglSwapBuffers) sys_eglSwapBuffers(g_Display, g_Surface);
}

EXTERNAL_API void pojavMakeCurrent(void* window) {
    (void)window;
    if (sys_eglMakeCurrent) sys_eglMakeCurrent(g_Display, g_Surface, g_Surface, g_Context);
}

EXTERNAL_API void pojavSwapInterval(int interval) {
    if (sys_eglSwapInterval) sys_eglSwapInterval(g_Display, interval);
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    // Ignore all hints
}

EXTERNAL_API void* pojavGetCurrentContext() {
    return (void*)g_Context;
}

EXTERNAL_API void pojavTerminate() {
    if (sys_eglMakeCurrent) sys_eglMakeCurrent(g_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (sys_eglDestroySurface) sys_eglDestroySurface(g_Display, g_Surface);
    if (sys_eglDestroyContext) sys_eglDestroyContext(g_Display, g_Context);
    if (sys_eglTerminate) sys_eglTerminate(g_Display);
}

// JNI Utils
JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_setupBridgeWindow(JNIEnv* env, ABI_COMPAT jclass clazz, jobject surface) {
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
}

JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass clazz) {
    if (pojav_environ->pojavWindow) ANativeWindow_release(pojav_environ->pojavWindow);
}

EXTERNAL_API JNIEXPORT jint JNICALL Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(JNIEnv *env, jclass clazz) { return 0; }
