#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <gbm.h>
#include <EGL/egl.h>

#include <jni.h>

typedef struct DrmProperties {
    drmModePropertyPtr* properties;
    uint32_t count;
} DrmProperties_t;

typedef struct DisplayHandle {
    uint32_t connectorId;
    DrmProperties_t connectorProperties;
    drmModeModeInfo mode;

    uint32_t encoderId;

    uint32_t crtcId;
    DrmProperties_t crtcProperties;

    uint32_t planeId;
    DrmProperties_t planeProperties;

    int fd;
    struct gbm_device* device;
    struct gbm_surface* surface;
    EGLDisplay display;
    struct gbm_bo* previousBo;
    uint8_t doModeset;
} DisplayHandle_t;

static DisplayHandle_t* currentDisplayHandle = NULL;

static void freeDrmProperties(DrmProperties_t* properties) {
    if (properties->count) {
        free(properties->properties);
    }
}

static void freeDisplayHandle(DisplayHandle_t* handle) {
    if (currentDisplayHandle == handle) {
        currentDisplayHandle = NULL;
    }

    if (handle->display != EGL_NO_DISPLAY) {
        eglTerminate(handle->display);
    }

    gbm_surface_destroy(handle->surface);
    gbm_device_destroy(handle->device);

    freeDrmProperties(&handle->connectorProperties);
    freeDrmProperties(&handle->crtcProperties);
    freeDrmProperties(&handle->planeProperties);

    close(handle->fd);
    free(handle);
}

static int getProperties(
        const char* displayId,
        int fd,
        uint32_t objectId,
        uint32_t objectType,
        DrmProperties_t* properties) {
    properties->count = 0;

    drmModeObjectPropertiesPtr objectProperties = drmModeObjectGetProperties(fd, objectId, objectType);
    if (!objectProperties) {
        fprintf(stderr, "Failed to get object properties(display id: %s, object id: %i): %s\n",
                displayId, objectId, strerror(errno));
        return -1;
    }

    if (!objectProperties->count_props) {
        drmModeFreeObjectProperties(objectProperties);
        return 0;
    }

    properties->properties = malloc(sizeof (drmModePropertyPtr) * objectProperties->count_props);
    if (!properties) {
        fprintf(stderr, "Failed to allocate object properties array\n");
        goto err_properties;
    }

    for (uint32_t i = 0; i < objectProperties->count_props; ++i) {
        properties->properties[i] = drmModeGetProperty(fd, objectProperties->props[i]);
        if (!properties->properties[i]) {
            fprintf(stderr, "Failed to get object property(display id: %s, object id: %i, property index: %i): %s\n",
                    displayId, objectId, i, strerror(errno));

            for (i = i - 1; i >= 0; --i) {
                drmModeFreeProperty(properties->properties[i]);
            }
            goto err_malloc;
        }
    }

    properties->count = objectProperties->count_props;
    return 0;

err_malloc:
    free(properties->properties);
err_properties:
    drmModeFreeObjectProperties(objectProperties);
    return -1;
}

static drmModeConnectorPtr findConnectedConnector(const char* displayId, int fd, drmModeResPtr resources) {
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnectorPtr connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (!connector) {
            fprintf(stderr, "drmModeGetConnector for %s and connector id %d failed: %s\n",
                    displayId, resources->connectors[i], strerror(errno));
            continue;
        }

        if (connector->connection == DRM_MODE_CONNECTED) {
            // TODO: Should we blindly pick up first available connector? Maybe check for environment
            //  variable or Java system property?
            if (connector->count_modes > 0) {
                // We're only intrested in connectors with at least one mode available.
                return connector;
            }
        }

        drmModeFreeConnector(connector);
    }

    // TODO: check for DRM_MODE_UNKNOWNCONNECTION connectors.
    return NULL;
}

static drmModeModeInfoPtr findPreferredMode(drmModeConnectorPtr connector) {
    // NB: Use mode with highest resolution if no preferred mode found.
    int chosenPixelsCount = 0;
    drmModeModeInfoPtr chosenMode = NULL;

    for (int i = 0; i < connector->count_modes; ++i) {
        drmModeModeInfoPtr mode = &connector->modes[i];

        if (mode->type & DRM_MODE_TYPE_PREFERRED) {
            return mode;
        }

        const int pixelsCount = mode->hdisplay * mode->vdisplay;
        if (pixelsCount > chosenPixelsCount) {
            chosenPixelsCount = pixelsCount;
            chosenMode = mode;
        }
    }

    return chosenMode;
}

static drmModeEncoderPtr findEncoder(
        const char *displayId,
        int fd,
        drmModeResPtr resources,
        drmModeConnectorPtr connector) {
    uint32_t encoderId = connector->encoder_id;

    for (int i = 0; i < resources->count_encoders; ++i) {
        if (resources->encoders[i] != encoderId) {
            continue;
        }

        drmModeEncoderPtr encoder = drmModeGetEncoder(fd, encoderId);
        if (!encoder) {
            fprintf(stderr, "drmModeGetEncoder for %s and encoder id %d failed: %s\n",
                    displayId, encoderId, strerror(errno));
            return NULL;
        }

        return encoder;
    }

    fprintf(stderr, "Failed to find encoder for connector (display id: %s, connector id: %d, encoder id: %d)\n",
            displayId, connector->connector_id, encoderId);

    return NULL;
}

static drmModeCrtcPtr findCrtc(
        const char *displayId,
        int fd,
        drmModeResPtr resources,
        drmModeEncoderPtr encoder) {
    uint32_t crtcId = encoder->crtc_id;

    for (int i = 0; i < resources->count_crtcs; ++i) {
        if (resources->crtcs[i] != crtcId) {
            continue;
        }

        drmModeCrtcPtr crtc = drmModeGetCrtc(fd, crtcId);
        if (!crtc) {
            fprintf(stderr, "drmModeGetCrtc for %s and CRTC id %d failed: %s\n",
                    displayId, crtcId, strerror(errno));
            return NULL;
        }
        return crtc;
    }

    fprintf(stderr, "Failed to find CRTC for encoder (display id: %s, encoder id: %d, CRTC id: %d)\n",
            displayId, encoder->encoder_id, crtcId);

    return NULL;
}

/**
 * Get a handle to the native window (without specifying what window is)
 *
 * This one should return a handle (an opaque pointer) that will be passed to |doEglCreateWindowSurface| as third
 * argument.
 */
jlong getNativeWindowHandle(const char* displayId) {
    if (!displayId) {
        goto err;
    }

    int fd = open(displayId, O_RDWR);

    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", displayId, strerror(errno));
        goto err;
    }

    drmModeResPtr resources = drmModeGetResources(fd);
    if (!resources) {
        if (errno == EOPNOTSUPP) {
            fprintf(stderr, "%s is not a valid display id: %s\n", displayId, strerror(errno));
        } else {
            fprintf(stderr, "drmModeGetResources for %s failed: %s\n", displayId, strerror(errno));
        }
        goto err_close_fd;
    }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "Atomic modesetting is not supported by display with id %s\n", displayId);
        goto err_close_fd;
    }

    drmModeConnectorPtr connector = findConnectedConnector(displayId, fd, resources);
    if (!connector) {
        goto err_free_resources;
    }

    drmModeModeInfoPtr mode = findPreferredMode(connector);
    assert(mode);

    DrmProperties_t connectorProperties;
    if (getProperties(displayId, fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR, &connectorProperties)) {
        goto err_free_connector;
    }

    drmModeEncoderPtr encoder = findEncoder(displayId, fd, resources, connector);
    if (!encoder) {
        goto err_free_connector;
    }

    if (!encoder->crtc_id) {
        fprintf(stderr, "No CRTC for encoder with id %d (display id: %s)\n", encoder->encoder_id, displayId);
        goto err_free_encoder;
    }

    drmModeCrtcPtr crtc = findCrtc(displayId, fd, resources, encoder);
    if (!crtc) {
        goto err_free_encoder;
    }

    DrmProperties_t crtcProperties;
    if (getProperties(displayId, fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, &crtcProperties)) {
        goto err_free_crtc;
    }

    drmModeFreeResources(resources);

    // TODO: We're choosing plane that is currently active. It is not quite correct.
    drmModePlanePtr plane = NULL;
    {
        if (!crtc->buffer_id) {
            fprintf(stderr, "No framebuffer for CRTC with id %d (display id: %s)\n", crtc->crtc_id, displayId);
            goto err_free_crtc;
        }

        drmModePlaneResPtr planeResources = drmModeGetPlaneResources(fd);
        if (!planeResources) {
            fprintf(stderr, "drmModeGetPlaneResources for %s failed: %s\n", displayId, strerror(errno));
            drmModeFreePlaneResources(planeResources);
            goto err_free_crtc;
        }

        for (uint32_t i = 0; i < planeResources->count_planes; ++i) {
            drmModePlanePtr currentPlane = drmModeGetPlane(fd, planeResources->planes[i]);
            if (!currentPlane) {
                fprintf(stderr, "drmModeGetPlane for %s and plane id %d failed: %s\n",
                        displayId, planeResources->planes[i], strerror(errno));
                continue;
            }

            if (currentPlane->crtc_id == crtc->crtc_id &&
                currentPlane->fb_id == crtc->buffer_id) {
                    plane = currentPlane;
                    break;
            }

            drmModeFreePlane(currentPlane);
        }

        drmModeFreePlaneResources(planeResources);

        if (!plane) {
            fprintf(stderr, "Failed to find plane for framebuffer with id %d (display id: %s)\n",
                    crtc->crtc_id, displayId);
            goto err_free_crtc;
        }
    }

    DrmProperties_t planeProperties;
    if (getProperties(displayId, fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, &planeProperties)) {
        goto err_free_plane;
    }

    struct gbm_device* gbmDevice = gbm_create_device(fd);
    if (!gbmDevice) {
        fprintf(stderr, "Failed to create GBM device for display with id %s: %s\n",
                displayId, strerror(errno));
        goto err_free_plane;
    }

    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    struct gbm_surface* surface = gbm_surface_create_with_modifiers(
                gbmDevice, mode->hdisplay, mode->vdisplay, DRM_FORMAT_ARGB8888, &modifier, 1
    );

    if (!surface) {
        fprintf(stderr, "Failed to create GBM surface for display with id %s: %s\n",
                displayId, strerror(errno));
        goto err_destroy_device;
    }

    DisplayHandle_t* handle = malloc(sizeof (DisplayHandle_t));
    if (!handle) {
        fprintf(stderr, "Failed to allocate NativeWindowHandle struct\n");
        goto err_destroy_surface;
    }

    handle->connectorId = connector->connector_id;
    handle->connectorProperties = connectorProperties;
    handle->encoderId = encoder->encoder_id;
    handle->crtcId = crtc->crtc_id;
    handle->crtcProperties = crtcProperties;
    handle->planeId = plane->plane_id;
    handle->planeProperties = planeProperties;
    handle->fd = fd;
    handle->device = gbmDevice;
    handle->surface = surface;
    handle->display = EGL_NO_DISPLAY;
    handle->previousBo = NULL;
    handle->doModeset = 1;

    memcpy(&handle->mode, mode, sizeof (drmModeModeInfo));

    currentDisplayHandle = handle;

    drmModeFreePlane(plane);
    drmModeFreeCrtc(crtc);
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);

    return (jlong) surface;

err_destroy_surface:
    gbm_surface_destroy(surface);
err_destroy_device:
    gbm_device_destroy(gbmDevice);
err_free_plane:
    drmModeFreePlane(plane);
err_free_crtc:
    drmModeFreeCrtc(crtc);
err_free_encoder:
    drmModeFreeEncoder(encoder);
err_free_connector:
    drmModeFreeConnector(connector);
err_free_resources:
    drmModeFreeResources(resources);
err_close_fd:
    close(fd);
err:
    return (jlong) NULL;
}

/**
 * Get a handle to the EGL display
 */
jlong getEglDisplayHandle() {
    DisplayHandle_t* handle = currentDisplayHandle;

    if (!handle) {
        return (jlong) NULL;
    }

    if (handle->display == EGL_NO_DISPLAY) {
        handle->display = eglGetDisplay((EGLNativeDisplayType) currentDisplayHandle->device);
    }

    if (handle->display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        freeDisplayHandle(handle);
        return (jlong) NULL;
    }

    return (jlong) handle;
}

/**
 * Initialize the EGL system with the specified handle
 */
jboolean doEglInitialize(void* displayHandle) {
    if (!displayHandle) {
        return JNI_FALSE;
    }

    DisplayHandle_t* handle = (DisplayHandle_t*) displayHandle;
    EGLBoolean result = eglInitialize(handle->display, NULL, NULL);

    if (result == EGL_FALSE) {
        fprintf(stderr, "EGL initialization failed\n");
        freeDisplayHandle(handle);
    }

    return result;
}

/**
 * Bind a specific API to the EGL system
 */
jboolean doEglBindApi(int api) {
    EGLBoolean result = eglBindAPI(api);

    if (result == EGL_FALSE) {
        fprintf(stderr, "Failed to bind EGL API\n");
    }

    return result;
}

/**
 * Instruct the system to choose an EGL configuration matching the provided attributes
 */
jlong doEglChooseConfig(jlong eglDisplay, int* attribs) {
    DisplayHandle_t* handle = (DisplayHandle_t*) eglDisplay;
    if (!handle) {
        return -1;
    }

    EGLDisplay display = handle->display;

    EGLint configsCount;

    if (!eglGetConfigs(display, NULL, 0, &configsCount)) {
        fprintf(stderr, "Failed to get EGL configurations count\n");
        goto err;
    }

    EGLConfig* configs = malloc(sizeof (EGLConfig) * configsCount);
    if (!configs) {
        fprintf(stderr, "Failed to allocate EGL configerations array\n");
        goto err;
    }

    // See com.sun.prism.es2.GLPixelFormat.Attributes for attribs pointer indices mapping.
    const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, attribs[6] != 0 ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
        EGL_RED_SIZE, attribs[0],
        EGL_GREEN_SIZE, attribs[1],
        EGL_BLUE_SIZE, attribs[2],
        EGL_ALPHA_SIZE, attribs[3],
        EGL_DEPTH_SIZE, attribs[4],
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint chosenCount = 0;
    if (eglChooseConfig(display, configAttributes, configs, configsCount, &chosenCount) == EGL_FALSE || !chosenCount) {
        fprintf(stderr, "Failed to choose EGL configuration\n");
        free(configs);
        goto err;
    }

    EGLConfig config = NULL;

    for (EGLint i = 0; i < chosenCount; ++i) {
        EGLint nativeVisualId;

        if (eglGetConfigAttrib(display, configs[i], EGL_NATIVE_VISUAL_ID, &nativeVisualId) == EGL_FALSE) {
                fprintf(stderr, "Failed to get EGL framebuffer configuration\n");
                continue;
        }

        if (nativeVisualId == DRM_FORMAT_ARGB8888) {
            config = configs[i];
            break;
        }
    }

    free(configs);

    if (!config) {
        fprintf(stderr, "Failed to find EGL configuration\n");
        goto err;
    }

    return (jlong) config;
err:
    freeDisplayHandle(handle);
    return -1;
}

/**
 * Create an EGL Surface for the given display, configuration and window
 */
jlong doEglCreateWindowSurface(jlong eglDisplay, jlong eglConfig, jlong eglNativeWindow) {
    DisplayHandle_t* handle = (DisplayHandle_t*) eglDisplay;
    if (!handle) {
        return (jlong) EGL_NO_SURFACE;
    }

    EGLConfig config = (EGLConfig) eglConfig;
    EGLNativeWindowType nativeWindow = (EGLNativeWindowType) eglNativeWindow;

    EGLSurface surface = eglCreateWindowSurface(handle->display, config, nativeWindow, NULL);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL window surface\n");
        freeDisplayHandle(handle);
    }

    return (jlong) surface;
}

/**
 * Create an EGL Context for the given display and configuration
 */
jlong doEglCreateContext(jlong eglDisplay, jlong eglConfig) {
    DisplayHandle_t* handle = (DisplayHandle_t*) eglDisplay;
    if (!handle) {
        return (jlong) EGL_NO_CONTEXT;
    }

    EGLConfig config = (EGLConfig) eglConfig;

    static const EGLint contextAttributes[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
    };

    EGLContext context = eglCreateContext(handle->display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        freeDisplayHandle(handle);
    }

    return (jlong) context;
}

/**
 * Enable the specified EGL system
 */
jboolean doEglMakeCurrent(jlong eglDisplay, jlong eglDrawSurface, jlong eglReadSurface, jlong eglContext) {
    DisplayHandle_t* handle = (DisplayHandle_t*) eglDisplay;
    if (!handle) {
        return JNI_FALSE;
    }

    EGLSurface drawSurface = (EGLDisplay) eglDrawSurface;
    EGLSurface readSurface = (EGLDisplay) eglReadSurface;
    EGLContext context = (EGLContext) eglContext;

    EGLBoolean result = eglMakeCurrent(handle->display, drawSurface, readSurface, context);
    if (result == EGL_FALSE) {
        fprintf(stderr, "eglMakeCurrent failed\n");
        freeDisplayHandle(handle);
    }

    return result;
}

typedef struct BoAndFramebuffer {
        struct gbm_bo *bo;
        uint32_t framebufferId;
} BoAndFramebuffer_t;

static void boAndFramebufferDestructor(struct gbm_bo* bo, void* data) {
    int drmFd = gbm_device_get_fd(gbm_bo_get_device(bo));
    BoAndFramebuffer_t* boAndFramebuffer = data;

    if (boAndFramebuffer->framebufferId) {
        drmModeRmFB(drmFd, boAndFramebuffer->framebufferId);
    }

    free(boAndFramebuffer);
}

static BoAndFramebuffer_t* getOrCreateBoAndFramebuffer(struct gbm_bo* bo) {
    int drmFd = gbm_device_get_fd(gbm_bo_get_device(bo));
    BoAndFramebuffer_t* boAndFramebuffer = gbm_bo_get_user_data(bo);

    if (boAndFramebuffer) {
        return boAndFramebuffer;
    }

    boAndFramebuffer = malloc(sizeof (BoAndFramebuffer_t));
    boAndFramebuffer->bo = bo;

    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t format = gbm_bo_get_format(bo);

    uint64_t modifiers[4] = {0};
    modifiers[0] = gbm_bo_get_modifier(bo);

    uint32_t strides[4] = {0};
    uint32_t handles[4] = {0};
    uint32_t offsets[4] = {0};

    const int planesCount = gbm_bo_get_plane_count(bo);

    for (int i = 0; i < planesCount; ++i) {
        handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = modifiers[0];
    }

    uint32_t flags = 0;

    if (modifiers[0] && modifiers[0] != DRM_FORMAT_MOD_INVALID) {
        flags = DRM_MODE_FB_MODIFIERS;
    }

    int result = drmModeAddFB2WithModifiers(drmFd, width, height, format, handles, strides, offsets, modifiers,
                                            &boAndFramebuffer->framebufferId, flags);

    if (result) {
        fprintf(stderr, "Failed to create framebuffer: %s\n", strerror(errno));
        free(boAndFramebuffer);
        return NULL;
    }

    gbm_bo_set_user_data(bo, boAndFramebuffer, boAndFramebufferDestructor);

    return boAndFramebuffer;
}

static int addProperty(
        drmModeAtomicReqPtr request,
        DrmProperties_t* properties,
        uint32_t objectId,
        const char *name,
        uint64_t value) {
    int propertyId = -1;

    for (uint32_t i = 0; i < properties->count; ++i) {
        if (strcmp(properties->properties[i]->name, name) == 0) {
            propertyId = properties->properties[i]->prop_id;
            break;
        }
    }

    if (propertyId < 0) {
        fprintf(stderr, "Failed to find property \"%s\" for object id %i\n", name, objectId);
        return -1;
    }

    if (drmModeAtomicAddProperty(request, objectId, propertyId, value) < 0) {
        fprintf(stderr, "Failed to set property \"%s\" (id: %i) for object id %i: %s\n",
                name, propertyId, objectId, strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * Swap buffers (and render frontbuffer)
 */
jboolean doEglSwapBuffers(jlong eglDisplay, jlong eglSurface) {
    DisplayHandle_t* handle = (DisplayHandle_t*) eglDisplay;
    if (!handle) {
        return JNI_FALSE;
    }

    EGLSurface surface = (EGLSurface) eglSurface;
    if (eglSwapBuffers(handle->display, surface) == EGL_FALSE) {
        fprintf(stderr, "eglSwapBuffers failed\n");
        return JNI_FALSE;
    }

    struct gbm_bo* nextBo = gbm_surface_lock_front_buffer(handle->surface);
    if (!nextBo) {
        fprintf(stderr, "Failed to lock surface front buffer: %s\n", strerror(errno));
        return JNI_FALSE;
    }

    BoAndFramebuffer_t* boAndFramebuffer = getOrCreateBoAndFramebuffer(nextBo);
    if (!boAndFramebuffer) {
        fprintf(stderr, "Failed to get framebuffer for buffer object\n");
        goto err_release_buffer;
    }

    drmModeAtomicReqPtr request = drmModeAtomicAlloc();

    int flags = 0;
    if (handle->doModeset) {
        flags &= DRM_MODE_ATOMIC_ALLOW_MODESET;
        if (addProperty(request, &handle->connectorProperties, handle->connectorId, "CRTC_ID", handle->crtcId) < 0) {
            goto err_free_request;
        }

        uint32_t blobId;
        if (drmModeCreatePropertyBlob(handle->fd, &handle->mode, sizeof (handle->mode), &blobId) != 0) {
            goto err_free_request;
        }

        if (addProperty(request, &handle->crtcProperties, handle->crtcId, "MODE_ID", blobId) < 0) {
            goto err_free_request;
        }

        if (addProperty(request, &handle->crtcProperties, handle->crtcId, "ACTIVE", 1) < 0) {
            goto err_free_request;
        }
    }

    addProperty(request, &handle->planeProperties, handle->planeId, "FB_ID", boAndFramebuffer->framebufferId);
    addProperty(request, &handle->planeProperties, handle->planeId, "CRTC_ID", handle->crtcId);
    addProperty(request, &handle->planeProperties, handle->planeId, "SRC_X", 0);
    addProperty(request, &handle->planeProperties, handle->planeId, "SRC_Y", 0);
    addProperty(request, &handle->planeProperties, handle->planeId, "SRC_W", handle->mode.hdisplay << 16);
    addProperty(request, &handle->planeProperties, handle->planeId, "SRC_H", handle->mode.vdisplay << 16);
    addProperty(request, &handle->planeProperties, handle->planeId, "CRTC_X", 0);
    addProperty(request, &handle->planeProperties, handle->planeId, "CRTC_Y", 0);
    addProperty(request, &handle->planeProperties, handle->planeId, "CRTC_W", handle->mode.hdisplay);
    addProperty(request, &handle->planeProperties, handle->planeId, "CRTC_H", handle->mode.vdisplay);

    if (drmModeAtomicCommit(handle->fd, request, flags, NULL)) {
        fprintf(stderr, "Failed to commit DRM mode: %s\n", strerror(errno));
        goto err_free_request;
    }

    handle->doModeset = 0;

    drmModeAtomicFree(request);

    if (handle->previousBo) {
        gbm_surface_release_buffer(handle->surface, handle->previousBo);
    }
    handle->previousBo = nextBo;
    return JNI_TRUE;

err_free_request:
    drmModeAtomicFree(request);
err_release_buffer:
    gbm_surface_release_buffer(handle->surface, nextBo);
    return JNI_FALSE;
}

/**
 * Get the number of native screens in the current configuration
 */
jint doGetNumberOfScreens() {
    // Only one screen is upported for now.
    return 1;
}

/**
 * Get screen handle
 */
jlong doGetHandle(jint idx) {
    // This one is used to determine screen uniqueness. We can use this to expose pointer to |DisplayHandle_t| to Java
    //  and pass it around. This also can be used to implement display hotplug support. Just return display index as
    //  handle for now.
    return idx;
}

/**
 * Get screen depth
 */
jint doGetDepth(jint idx) {
    if (idx > 0) {
        return 0;
    }

    // Depth is always 32 for GBM_FORMAT_ARGB8888
    return 32;
}

jfloat doGetScale(jint idx);

/**
 * Get screen width
 */
jint doGetWidth(jint idx) {
    if (idx > 0) {
        return 0;
    }

    DisplayHandle_t* handle = currentDisplayHandle;
    if (!handle) {
        return 0;
    }

    return (float) handle->mode.hdisplay / doGetScale(idx);
}

/**
 * Get screen height
 */
jint doGetHeight(jint idx) {
    if (idx > 0) {
        return 0;
    }

    DisplayHandle_t* handle = currentDisplayHandle;
    if (!handle) {
        return 0;
    }

    return (float) handle->mode.vdisplay / doGetScale(idx);
}

/**
 * Get screen offset for X axis
 */
jint doGetOffsetX(jint idx) {
    (void) idx;

    // Offset os always 0 for screen 0.
    return 0;
}

/*
 * Get screen offset for Y axis
 */
jint doGetOffsetY(jint idx) {
    (void) idx;

    // Offset os always 0 for screen 0.
    return 0;
}

/**
 * Get screen DPI
 */
jint doGetDpi(jint idx) {
    if (idx > 0) {
        return 0;
    }

    // TODO: Return actual DPI. We can get it from display EDID, but this requires EDID parsing.
    return 96;
}

/**
 * Get screen native format
 */
jint doGetNativeFormat(jint idx) {
    if (idx > 0) {
        return 0;
    }

    // TODO: Figure out where it is used and is is this correct.
    // com.sun.glass.ui.Pixels.Format#BYTE_BGRA_PRE
    return 1;
}

/**
 * Get screen scale
 */
jfloat doGetScale(jint idx) {
    (void) idx;

    return SCALE_FACTOR;
}

// TODO: We can actually implement cursor ourelves using free plane. This will allow us to show cursor on systems
//  without cursor plane. But there is DRM side cursor handling implementation which is simplier to use. Use DRM side
//  cursor handling for now.
static struct CursorState {
    uint32_t width;
    uint32_t height;
    struct gbm_bo* cursorBo;
    uint32_t boHandle;
    uint8_t visible;
} cursorState = {
    .width = 0,
    .height = 0,
    .cursorBo = NULL,
    .boHandle = 0,
    .visible = 0
};

/**
 * Initialize a hardware cursor with specified dimensions
 */
void doInitCursor(jint width, jint height) {
    cursorState.width = width;
    cursorState.height = height;
}

/**
 * Show/hide the hardware cursor
 */
void doSetCursorVisibility(jboolean visible) {
    DisplayHandle_t* handle = currentDisplayHandle;
    if (!handle) {
        return;
    }

    cursorState.visible = visible;
    uint32_t boHandle = visible ? cursorState.boHandle : 0;

    int error = drmModeSetCursor(handle->fd, handle->crtcId, boHandle, cursorState.width, cursorState.height);
    if (error) {
        fprintf(stderr, "Failed to set cursor visibility: %s\n", strerror(errno));
    }
}

/**
 * Point the hardware cursor to the provided location
 */
void doSetLocation(jint x, jint y) {
    DisplayHandle_t* handle = currentDisplayHandle;
    if (!handle) {
        return;
    }

    x *= doGetScale(0);
    y *= doGetScale(0);

    int error = drmModeMoveCursor(handle->fd, handle->crtcId, x, y);
    if (error) {
        fprintf(stderr, "Failed to move cursor: %s\n", strerror(errno));
    }
}

/**
 * use the specified image as cursor image
 */
void doSetCursorImage(jbyte* img, int length) {
    DisplayHandle_t* handle = currentDisplayHandle;
    if (!handle) {
        return;
    }

    struct gbm_bo* cursorBo = gbm_bo_create(handle->device, cursorState.width, cursorState.height, GBM_FORMAT_ARGB8888,
                                            GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
    if (!cursorBo) {
        fprintf(stderr, "Failed to create cursor buffer object: %s\n", strerror(errno));
        return;
    }

    uint32_t stride;
    void* mapData = NULL;
    char* map =
            gbm_bo_map(cursorBo, 0, 0, cursorState.width, cursorState.height, GBM_BO_TRANSFER_WRITE, &stride, &mapData);

    if (!map) {
        fprintf(stderr, "Failed to map cursor buffer object: %s\n", strerror(errno));
        goto err_destroy_bo;
    }

    for (uint32_t i = 0; i < cursorState.height; ++i) {
        // TODO: Check cursor plane for pre-multiplication requirements.
#ifndef PRE_MULTIPLY_CURSOR
        if (cursorState.width * 4 * (i + 1) >= (uint32_t) length) {
            goto outer_break;
        }

        memcpy(&map[stride * i], &img[cursorState.width * 4 * i], cursorState.width * 4);
#else
        // NB: Pre-multiply incoming image to make it blend correctly.
        for (uint32_t j = 0; j < cursorState.width; ++j) {
            // Do a sanity check
            if (cursorState.width * 4 * i + j * 4 + 3 >= (uint32_t) length) {
                goto outer_break;
            }

            double alpha = ((uint8_t)img[cursorState.width * 4 * i + j * 4 + 3]) / 255.;

            map[stride * i + j * 4    ] = ((uint8_t)img[cursorState.width * 4 * i + j * 4    ]) * alpha;
            map[stride * i + j * 4 + 1] = ((uint8_t)img[cursorState.width * 4 * i + j * 4 + 1]) * alpha;
            map[stride * i + j * 4 + 2] = ((uint8_t)img[cursorState.width * 4 * i + j * 4 + 2]) * alpha;
            map[stride * i + j * 4 + 3] = img[cursorState.width * 4 * i + j * 4 + 3];
        }
#endif
    }

outer_break:
    gbm_bo_unmap(cursorBo, mapData);

    if (cursorState.cursorBo) {
        gbm_bo_destroy(cursorState.cursorBo);
    }

    cursorState.cursorBo = cursorBo;
    cursorState.boHandle = gbm_bo_get_handle(cursorBo).u32;

    if (cursorState.visible) {
        int error = drmModeSetCursor(
                    handle->fd, handle->crtcId, cursorState.boHandle, cursorState.width, cursorState.height
        );
        if (error) {
            fprintf(stderr, "Failed to update cursor image: %s\n", strerror(errno));
        }
    }

    return;
err_destroy_bo:
    gbm_bo_destroy(cursorBo);
}
