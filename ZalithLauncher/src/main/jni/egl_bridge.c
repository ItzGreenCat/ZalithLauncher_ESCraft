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

// 必须包含此头文件
#include <environ/environ.h>

#include <EGL/egl.h>
#include "ctxbridges/egl_loader.h"
#include "ctxbridges/osmesa_loader.h"
#include "ctxbridges/renderer_config.h"
#include "ctxbridges/virgl_bridge.h"
#include "driver_helper/nsbypass.h"
#include "utils.h"
#include "ctxbridges/bridge_tbl.h"
#include "ctxbridges/osm_bridge.h"

// -------------------------------------------------------------
// Macros
// -------------------------------------------------------------
#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
#define GLFW_OPENGL_ES_API 0x30002

#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

// 初始化全局变量
EXTERNAL_API EGLConfig config = NULL;
struct PotatoBridge potatoBridge;
void* loadTurnipVulkan(); 
void calculateFPS();

// -------------------------------------------------------------
// Core Logic: System GLES
// -------------------------------------------------------------

static void force_system_gles_drivers() {
    printf("EGLBridge: [INFO] Configuring for System GLES Drivers...\n");

    const char* lib_egl = "libEGL.so";
    const char* lib_gles = "libGLESv2.so";

    setenv("LIBGL_EGL", lib_egl, 1);
    setenv("LIBGL_GLES", lib_gles, 1);
    setenv("LIBGL_ES", lib_gles, 1);
    
    unsetenv("GALLIUM_DRIVER"); 
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");

    printf("EGLBridge: Drivers target: %s, %s\n", lib_egl, lib_gles);

    set_gl_bridge_tbl();
}

static void force_bind_gles_api() {
    // [FIX] 定义函数指针类型
    typedef EGLBoolean (*eglBindAPI_t)(EGLenum);
    typedef EGLint (*eglGetError_t)(void);

    // 动态加载符号，避免链接错误
    eglBindAPI_t ptr_eglBindAPI = (eglBindAPI_t)dlsym(RTLD_DEFAULT, "eglBindAPI");
    eglGetError_t ptr_eglGetError = (eglGetError_t)dlsym(RTLD_DEFAULT, "eglGetError");
    
    // 如果找不到，尝试手动 dlopen 系统库
    if (!ptr_eglBindAPI) {
        void* handle = dlopen("libEGL.so", RTLD_LAZY);
        if (handle) {
            ptr_eglBindAPI = (eglBindAPI_t)dlsym(handle, "eglBindAPI");
            if (!ptr_eglGetError) ptr_eglGetError = (eglGetError_t)dlsym(handle, "eglGetError");
        }
    }

    if (ptr_eglBindAPI) {
        printf("EGLBridge: FORCE calling eglBindAPI(EGL_OPENGL_ES_API)...\n");
        // 0x30A0 = EGL_OPENGL_ES_API
        if (ptr_eglBindAPI(0x30A0)) {
            printf("EGLBridge: API Bind Success.\n");
        } else {
            // [FIX] 使用动态加载的指针调用 eglGetError，或者 fallback 到 0
            EGLint err = (ptr_eglGetError) ? ptr_eglGetError() : 0;
            printf("EGLBridge: API Bind Failed (eglGetError: 0x%x)\n", err);
        }
    } else {
        printf("EGLBridge: WARNING - eglBindAPI symbol not found. EGL might default to Desktop GL.\n");
    }
}

void load_vulkan() {
    if(getenv("VULKAN_PTR") == NULL) {
         void* vPtr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
         if (vPtr) {
             char envval[64];
             sprintf(envval, "%"PRIxPTR, (uintptr_t)vPtr);
             setenv("VULKAN_PTR", envval, 1);
         }
    }
}

EXTERNAL_API void* maybe_load_vulkan() {
    if(getenv("VULKAN_PTR") == NULL) load_vulkan();
    const char* ptrStr = getenv("VULKAN_PTR");
    if (ptrStr == NULL) return NULL;
    return (void*) strtoul(ptrStr, NULL, 0x10);
}

// -------------------------------------------------------------
// Initialization
// -------------------------------------------------------------

int pojavInitOpenGL() {
    const char *forceVsync = getenv("FORCE_VSYNC");
    if (forceVsync && !strcmp(forceVsync, "true"))
        pojav_environ->force_vsync = true;

    maybe_load_vulkan();

    pojav_environ->config_renderer = RENDERER_GL4ES;
    
    force_system_gles_drivers();
    
    if (br_init()) {
        br_setup_window();
    } else {
        printf("EGLBridge: [FATAL] br_init() failed! Check Logcat for EGL errors.\n");
        return -1;
    }

    force_bind_gles_api();

    return 0;
}

EXTERNAL_API int pojavInit() {
    if (!pojav_environ->pojavWindow) {
        printf("EGLBridge: [ERROR] pojavWindow is NULL. Window setup skipped?\n");
        return 0;
    }

    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow, 
                                     pojav_environ->savedWidth, 
                                     pojav_environ->savedHeight, 
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    
    if (pojavInitOpenGL() != 0) {
        return 0; 
    }
    return 1; 
}

// -------------------------------------------------------------
// Window Hint Interceptor
// -------------------------------------------------------------
EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;
    switch (value) {
        case GLFW_NO_API:
            pojav_environ->config_renderer = RENDERER_VULKAN;
            break;
        case GLFW_OPENGL_API:
             printf("EGLBridge: [HINT] Java requested Desktop GL. Overriding logic will force GLES.\n");
             break;
        case GLFW_OPENGL_ES_API:
             printf("EGLBridge: [HINT] Java requested GLES. Good.\n");
             break;
        default: break;
    }
}

// -------------------------------------------------------------
// Context & Utils
// -------------------------------------------------------------

EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    void* ctx = br_init_context((basic_render_window_t*)contextSrc);
    if (!ctx) {
        printf("EGLBridge: [FATAL] pojavCreateContext returned NULL. Config mismatch?\n");
    }
    return ctx;
}

EXTERNAL_API void pojavTerminate() {
    if (potatoBridge.eglDisplay) {
        eglMakeCurrent_p(potatoBridge.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (potatoBridge.eglSurface) eglDestroySurface_p(potatoBridge.eglDisplay, potatoBridge.eglSurface);
        if (potatoBridge.eglContext) eglDestroyContext_p(potatoBridge.eglDisplay, potatoBridge.eglContext);
        eglTerminate_p(potatoBridge.eglDisplay);
        eglReleaseThread_p();
    }
    memset(&potatoBridge, 0, sizeof(potatoBridge));
}
EXTERNAL_API void pojavSwapBuffers() { calculateFPS(); br_swap_buffers(); }
EXTERNAL_API void pojavMakeCurrent(void* window) { br_make_current((basic_render_window_t*)window); }
EXTERNAL_API void pojavSwapInterval(int interval) { br_swap_interval(interval); }
JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_setupBridgeWindow(JNIEnv* env, ABI_COMPAT jclass clazz, jobject surface) {
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
    if (br_setup_window) br_setup_window();
}
JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass clazz) {
    if (pojav_environ->pojavWindow) ANativeWindow_release(pojav_environ->pojavWindow);
}
EXTERNAL_API void* pojavGetCurrentContext() { return br_get_current(); }
static int frameCount = 0; static int fps = 0; static time_t lastTime = 0;
void calculateFPS() {
    frameCount++; time_t currentTime = time(NULL);
    if (currentTime != lastTime) { lastTime = currentTime; fps = frameCount; frameCount = 0; }
}
EXTERNAL_API JNIEXPORT jint JNICALL Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(JNIEnv *env, jclass clazz) { return fps; }
EXTERNAL_API JNIEXPORT jlong JNICALL Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    return (jlong) maybe_load_vulkan();
}
