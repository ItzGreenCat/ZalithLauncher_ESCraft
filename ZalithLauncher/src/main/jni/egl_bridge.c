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

// [FIX 1] 包含环境头文件，解决 pojav_environ 未定义错误
#include <environ/environ.h>

// 引入内部头文件
#include <EGL/egl.h>
#include "ctxbridges/egl_loader.h"
#include "ctxbridges/osmesa_loader.h"
#include "ctxbridges/renderer_config.h"
#include "ctxbridges/virgl_bridge.h"
#include "driver_helper/nsbypass.h"
#include "utils.h"
#include "ctxbridges/bridge_tbl.h"
#include "ctxbridges/osm_bridge.h"

// =============================================================
// [HACK] 强制宏定义
// =============================================================
#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
#define GLFW_OPENGL_ES_API 0x30002

// [FIX 2] 定义可见性宏，解决链接错误
#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

// [FIX 3] 显式初始化全局变量并强制导出
EXTERNAL_API EGLConfig config = NULL;
struct PotatoBridge potatoBridge;

void* loadTurnipVulkan(); 
void calculateFPS();

// =============================================================
// [CORE] 强制系统驱动加载逻辑
// =============================================================
static void force_system_gles_drivers() {
    printf("EGLBridge: [NUCLEAR OPTION] Forcing System GLES Drivers...\n");

    const char* sys_lib_path;
    if (sizeof(void*) == 8) {
        sys_lib_path = "/system/lib64/";
    } else {
        sys_lib_path = "/system/lib/";
    }

    char path_egl[128];
    char path_gles[128];
    snprintf(path_egl, sizeof(path_egl), "%slibEGL.so", sys_lib_path);
    snprintf(path_gles, sizeof(path_gles), "%slibGLESv2.so", sys_lib_path);

    // 暴力覆盖环境变量
    setenv("LIBGL_EGL", path_egl, 1);
    setenv("LIBGL_GLES", path_gles, 1);
    setenv("LIBGL_ES", path_gles, 1);
    
    unsetenv("GALLIUM_DRIVER"); 
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");

    printf("EGLBridge: Redirected EGL to %s\n", path_egl);
    printf("EGLBridge: Redirected GLES to %s\n", path_gles);

    set_gl_bridge_tbl();
}

static void force_bind_gles_api() {
    // 动态查找并调用 eglBindAPI
    void (*ptr_eglBindAPI)(EGLenum);
    ptr_eglBindAPI = dlsym(RTLD_DEFAULT, "eglBindAPI");

    if (ptr_eglBindAPI) {
        // EGL_OPENGL_ES_API = 0x30A0
        printf("EGLBridge: FORCE calling eglBindAPI(EGL_OPENGL_ES_API)\n");
        ptr_eglBindAPI(0x30A0);
    } else {
        printf("EGLBridge: CRITICAL WARNING - eglBindAPI symbol not found!\n");
    }
}

// =============================================================
// 初始化逻辑
// =============================================================

// [FIX 4] 确保 load_vulkan 也是可见的，虽然它通常是内部使用
void load_vulkan() {
    const char* zinkPreferSystemDriver = getenv("POJAV_ZINK_PREFER_SYSTEM_DRIVER");
    int deviceApiLevel = android_get_device_api_level();
    if (zinkPreferSystemDriver == NULL && deviceApiLevel >= 28) {
#ifdef ADRENO_POSSIBLE
        void* result = loadTurnipVulkan();
        if (result != NULL)
        {
            printf("AdrenoSupp: Loaded Turnip, loader address: %p\n", result);
            
             char envval[64];
             sprintf(envval, "%"PRIxPTR, (uintptr_t)result);
             setenv("VULKAN_PTR", envval, 1);
            return;
        }
#endif
    }

    // Fallback loading
    if(getenv("VULKAN_PTR") == NULL) {
         void* vPtr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
         if (vPtr) {
             char envval[64];
             sprintf(envval, "%"PRIxPTR, (uintptr_t)vPtr);
             setenv("VULKAN_PTR", envval, 1);
         }
    }
}

// [FIX 5] 强制导出 maybe_load_vulkan，解决 lwjgl_dlopen_hook 链接错误
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

    // 确保 Vulkan 句柄已加载
    maybe_load_vulkan();

    // 强制走 GL4ES (EGL) 路径
    pojav_environ->config_renderer = RENDERER_GL4ES;

    // 执行强制加载
    force_system_gles_drivers();
    
    // 执行强制绑定
    force_bind_gles_api();

    if (br_init()) {
        br_setup_window();
    } else {
        printf("EGLBridge: Failed to initialize EGL!\n");
        return -1;
    }

    return 0;
}

EXTERNAL_API int pojavInit() {
    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow, 
                                     pojav_environ->savedWidth, 
                                     pojav_environ->savedHeight, 
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    
    return pojavInitOpenGL();
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;

    switch (value) {
        case GLFW_NO_API:
            pojav_environ->config_renderer = RENDERER_VULKAN;
            break;
        case GLFW_OPENGL_API:
             printf("EGLBridge: Java requested Desktop GL, forcing override to GLES path.\n");
             break;
        case GLFW_OPENGL_ES_API:
             printf("EGLBridge: Java requested GLES, proceeding.\n");
             break;
        default:
            printf("GLFW: Warning - Unknown Client API 0x%x, ignoring.\n", value);
    }
}

EXTERNAL_API void pojavTerminate() {
    printf("EGLBridge: Terminating\n");
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

EXTERNAL_API void pojavMakeCurrent(void* window) {
    br_make_current((basic_render_window_t*)window);
}

EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    return br_init_context((basic_render_window_t*)contextSrc);
}

EXTERNAL_API void pojavSwapInterval(int interval) {
    br_swap_interval(interval);
}

JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_setupBridgeWindow(JNIEnv* env, ABI_COMPAT jclass clazz, jobject surface) {
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
    if (br_setup_window) br_setup_window();
}

JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass clazz) {
    if (pojav_environ->pojavWindow) {
        ANativeWindow_release(pojav_environ->pojavWindow);
    }
}

EXTERNAL_API void* pojavGetCurrentContext() {
    return br_get_current();
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
EXTERNAL_API JNIEXPORT jint JNICALL Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(JNIEnv *env, jclass clazz) {
    return fps;
}

EXTERNAL_API JNIEXPORT jlong JNICALL Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    return (jlong) maybe_load_vulkan();
}
