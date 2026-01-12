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
#include "ctxbridges/egl_loader.h"
#include "ctxbridges/osmesa_loader.h"
#include "ctxbridges/renderer_config.h"
#include "ctxbridges/virgl_bridge.h"
#include "driver_helper/nsbypass.h"
#include "utils.h"
#include "ctxbridges/bridge_tbl.h"
#include "ctxbridges/osm_bridge.h"

// -------------------------------------------------------------
// Macros & Exports
// -------------------------------------------------------------
#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
#define GLFW_OPENGL_ES_API 0x30002

#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

EXTERNAL_API EGLConfig config = NULL;
struct PotatoBridge potatoBridge;
void* loadTurnipVulkan(); 
void calculateFPS();

// -------------------------------------------------------------
// Core Logic
// -------------------------------------------------------------

static void force_system_gles_drivers() {
    printf("EGLBridge: [SAFE MODE] Forcing System GLES Drivers...\n");

    // [FIX] 不使用绝对路径，直接使用文件名
    // Android Linker 会自动在 /system/lib64 或 /system/lib 中找到它们
    // 这样可以规避 Namespace Isolation 的权限问题
    const char* lib_egl = "libEGL.so";
    const char* lib_gles = "libGLESv2.so";

    setenv("LIBGL_EGL", lib_egl, 1);
    setenv("LIBGL_GLES", lib_gles, 1);
    setenv("LIBGL_ES", lib_gles, 1);
    
    // 清理干扰项
    unsetenv("GALLIUM_DRIVER"); 
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");

    printf("EGLBridge: Environment set to load %s and %s\n", lib_egl, lib_gles);

    // 加载符号表 (dlopen 将被触发)
    set_gl_bridge_tbl();
}

static void force_bind_gles_api() {
    void (*ptr_eglBindAPI)(EGLenum) = dlsym(RTLD_DEFAULT, "eglBindAPI");
    if (ptr_eglBindAPI) {
        printf("EGLBridge: FORCE calling eglBindAPI(EGL_OPENGL_ES_API)\n");
        ptr_eglBindAPI(0x30A0);
    } else {
        // 如果这里打印了 Warning，说明 libEGL.so 根本没加载成功
        printf("EGLBridge: WARNING - eglBindAPI symbol not found! EGL load failed?\n");
    }
}

void load_vulkan() {
    // 简化的 Vulkan 加载逻辑
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

int pojavInitOpenGL() {
    const char *forceVsync = getenv("FORCE_VSYNC");
    if (forceVsync && !strcmp(forceVsync, "true"))
        pojav_environ->force_vsync = true;

    maybe_load_vulkan();

    // 强制路径
    pojav_environ->config_renderer = RENDERER_GL4ES;
    force_system_gles_drivers();
    force_bind_gles_api();

    // 初始化 EGL
    // [CRITICAL] 如果这里失败，返回 -1
    if (br_init()) {
        br_setup_window();
        return 0; // Success
    } else {
        printf("EGLBridge: ERROR - br_init() failed. EGL could not be initialized.\n");
        return -1; // Fail
    }
}

EXTERNAL_API int pojavInit() {
    // 如果窗口还没准备好，等待一下或者直接报错
    if (!pojav_environ->pojavWindow) {
        printf("EGLBridge: Error - pojavWindow is NULL in pojavInit!\n");
    } else {
        ANativeWindow_acquire(pojav_environ->pojavWindow);
        pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
        pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
        ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow, 
                                         pojav_environ->savedWidth, 
                                         pojav_environ->savedHeight, 
                                         AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    }
    
    // [FIX] 如果初始化失败，返回 0 (GLFW_FALSE)
    if (pojavInitOpenGL() != 0) {
        return 0; 
    }
    return 1; // Success
}

// ... 下面保持不变 ...
EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;
    switch (value) {
        case GLFW_NO_API:
            pojav_environ->config_renderer = RENDERER_VULKAN;
            break;
        case GLFW_OPENGL_API:
             printf("EGLBridge: Java requested Desktop GL, overridden.\n");
             break;
        case GLFW_OPENGL_ES_API:
             printf("EGLBridge: Java requested GLES, OK.\n");
             break;
        default: break;
    }
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

EXTERNAL_API void pojavSwapBuffers() {
    calculateFPS();
    br_swap_buffers(); 
}
EXTERNAL_API void pojavMakeCurrent(void* window) { br_make_current((basic_render_window_t*)window); }
EXTERNAL_API void* pojavCreateContext(void* contextSrc) { return br_init_context((basic_render_window_t*)contextSrc); }
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
