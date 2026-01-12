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

// [FIX] 必须包含这个头文件，否则找不到 pojav_environ
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

#define EXTERNAL_API __attribute__((used))
#define ABI_COMPAT __attribute__((unused))

// 声明全局变量
struct PotatoBridge potatoBridge;
void* loadTurnipVulkan(); // 保留 Vulkan 加载器以防万一
void calculateFPS();

// =============================================================
// [CORE] 强制系统驱动加载逻辑
// =============================================================
static void force_system_gles_drivers() {
    printf("EGLBridge: [NUCLEAR OPTION] Forcing System GLES Drivers...\n");

    // 1. 检测架构路径
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

    // 2. 暴力覆盖环境变量
    // 无论之前 Java 层设置了什么，这里全部覆盖为系统路径
    setenv("LIBGL_EGL", path_egl, 1);
    setenv("LIBGL_GLES", path_gles, 1);
    setenv("LIBGL_ES", path_gles, 1);
    
    // 清除可能导致干扰的变量
    unsetenv("GALLIUM_DRIVER"); 
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");

    printf("EGLBridge: Redirected EGL to %s\n", path_egl);
    printf("EGLBridge: Redirected GLES to %s\n", path_gles);

    // 3. 加载符号表 (这将打开上面的系统库)
    // 我们复用 GL4ES 的 loader，因为它本质就是个 EGL Loader
    set_gl_bridge_tbl();
}

static void force_bind_gles_api() {
    // 4. 动态查找并调用 eglBindAPI
    // 这是防止回退到 Desktop GL 的最后一道防线
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

int pojavInitOpenGL() {
    // VSync 控制
    const char *forceVsync = getenv("FORCE_VSYNC");
    if (forceVsync && !strcmp(forceVsync, "true"))
        pojav_environ->force_vsync = true;

    // 尝试加载 Vulkan (仅用于获取句柄，不用于渲染)
    // 某些设备可能需要这个来唤醒 GPU
    if(getenv("VULKAN_PTR") == NULL) {
         void* vPtr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
         if (vPtr) {
             char envval[64];
             sprintf(envval, "%"PRIxPTR, (uintptr_t)vPtr);
             setenv("VULKAN_PTR", envval, 1);
         }
    }

    // [关键修改] 无视所有渲染器判断，直接走 EGL 路径
    // RENDERER_GL4ES 在这里仅仅代表 "使用 egl_loader.c 的逻辑"
    pojav_environ->config_renderer = RENDERER_GL4ES;

    // 执行强制加载
    force_system_gles_drivers();
    
    // 执行强制绑定
    force_bind_gles_api();

    // 初始化 EGL 上下文
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
    
    // 强制设置为 RGBA_8888，这是 GLES 最通用的格式
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow, 
                                     pojav_environ->savedWidth, 
                                     pojav_environ->savedHeight, 
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    
    return pojavInitOpenGL();
}

// =============================================================
// Window Hint (API 拦截)
// =============================================================
EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;

    // [关键修改] 无论 Java 请求 Desktop GL 还是 ES，全部放行
    // 因为我们在 init 里已经强制绑定了 eglBindAPI(ES)，
    // 所以即使 GLFW 请求 Desktop，得到的也会是 ES (或者报错，取决于 EGL 实现)
    // 但在这里拦截是为了防止 Native Bridge 报错 "Unimplemented"
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

// =============================================================
// 其他辅助函数 (保持精简)
// =============================================================

EXTERNAL_API void pojavTerminate() {
    printf("EGLBridge: Terminating\n");
    // 既然强制走了 GL4ES (EGL) 路径，这里只需要处理清理逻辑
    if (potatoBridge.eglDisplay) {
        eglMakeCurrent_p(potatoBridge.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (potatoBridge.eglSurface) eglDestroySurface_p(potatoBridge.eglDisplay, potatoBridge.eglSurface);
        if (potatoBridge.eglContext) eglDestroyContext_p(potatoBridge.eglDisplay, potatoBridge.eglContext);
        eglTerminate_p(potatoBridge.eglDisplay);
        eglReleaseThread_p();
    }
    memset(&potatoBridge, 0, sizeof(potatoBridge));
}

// 窗口交换
EXTERNAL_API void pojavSwapBuffers() {
    calculateFPS();
    br_swap_buffers(); // 直接调用 EGL swap
}

// Make Current
EXTERNAL_API void pojavMakeCurrent(void* window) {
    br_make_current((basic_render_window_t*)window);
}

// 上下文创建
EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    // 强制调用 EGL 创建
    return br_init_context((basic_render_window_t*)contextSrc);
}

// VSync
EXTERNAL_API void pojavSwapInterval(int interval) {
    br_swap_interval(interval);
}

// JNI 接口 - 窗口设置
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

// FPS 计数器
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

// Vulkan 句柄获取 (兼容性保留)
EXTERNAL_API JNIEXPORT jlong JNICALL Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    if(getenv("VULKAN_PTR") == NULL) {
         void* vPtr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
         return (jlong)vPtr;
    }
    return (jlong) strtoul(getenv("VULKAN_PTR"), NULL, 0x10);
}
