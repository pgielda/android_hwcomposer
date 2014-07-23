/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <hardware/hardware.h>

#include <fcntl.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>


#include <cutils/log.h>
#include <cutils/atomic.h>

#include <cutils/native_handle.h>


#include <hardware/hwcomposer.h>

#include <EGL/egl.h>

#include <time.h>

#include <linux/fb.h>


/*****************************************************************************/

struct hwc_context_t {
    hwc_composer_device_1_t device;
    hwc_procs_t const* procs;
    struct hw_module_t *gralloc;
    framebuffer_device_t *framebuffer;
    int fd;
    void *mappedAddress;
    /* our private state goes below here */
};

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    open: hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: HWC_HARDWARE_MODULE_ID,
        name: "Sample hwcomposer module",
        author: "The Android Open Source Project",
        methods: &hwc_module_methods,
    }
};

/*****************************************************************************/

const char *
comp_type_str(int32_t type)
{
    switch (type) {
        case HWC_BACKGROUND: return "BACKGROUND";
        case HWC_FRAMEBUFFER_TARGET: return "FB TARGET";
        case HWC_FRAMEBUFFER: return "FB";
        case HWC_OVERLAY: return "OVERLAY";
    }

    return "unknown";
}



static void dump_layer(hwc_layer_1_t const* l) {
    ALOGD("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x (%s), {%d,%d,%d,%d}, {%d,%d,%d,%d}, comp_type=%s",
            l->compositionType, l->flags, l->handle, l->transform, l->blending, (l->blending == 0x100) ? "BLENDING_NONE" : "BLENDING_ALPHA", 
            l->sourceCrop.left,
            l->sourceCrop.top,
            l->sourceCrop.right,
            l->sourceCrop.bottom,
            l->displayFrame.left,
            l->displayFrame.top,
            l->displayFrame.right,
            l->displayFrame.bottom,
	    comp_type_str(l->compositionType));
}

static int hwc_prepare(hwc_composer_device_1_t *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays) {
    ALOGD("we're in hwc_prepare");
    if (displays && (displays[0]->flags & HWC_GEOMETRY_CHANGED)) {
        for (size_t i=0 ; i<displays[0]->numHwLayers ; i++) {
            dump_layer(&displays[0]->hwLayers[i]);
	    // TODO: HWC_FRAMEBUFFER - handled by GLES
	    //       HWC_OVERLAY - handled by hwc
	    //       HWC_BACKGROUND - background
	    if ((i == 0) && (displays[0]->hwLayers[0].blending == HWC_BLENDING_NONE)) {
	            displays[0]->hwLayers[0].compositionType = HWC_OVERLAY; // let's remove the background ;)
	    } else {
		    displays[0]->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
	    }
        }
    }
    return 0;
}

static int hwc_set(hwc_composer_device_1_t *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
 struct hwc_context_t* ctx = (struct hwc_context_t*)dev;

    ALOGD("hwc_set, layers=%d", displays[0]->numHwLayers);
//    for (size_t i=0 ; i<displays[0]->numHwLayers ; i++) {
  //      dump_layer(&displays[0]->hwLayers[i]);
   // } 
    ALOGD("swapping buffers\n");
    EGLBoolean sucess = eglSwapBuffers((EGLDisplay)displays[0]->dpy,
            (EGLSurface)displays[0]->sur);
    if (!sucess) {
    	ALOGD("something is wrong, returning error!");
        return HWC_EGL_ERROR;
    }

    for (int i = 0; i < displays[0]->numHwLayers; i++) {
    	if (displays[0]->hwLayers[i].compositionType == HWC_OVERLAY) {
		 ALOGD("hwcomposer should render this layer (%d):", i);
		 dump_layer(&displays[0]->hwLayers[i]);
		 memset(ctx->mappedAddress, 0x80, 2000);
	}

    }

    return 0;
}

static int hwc_device_close(struct hw_device_t *dev)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;
    ALOGD("hwc_device_close");
    if (ctx) {
        free(ctx);
    }
    return 0;
}

uint64_t tm = 0;

int64_t systemTime()
{
    struct timespec t;
    t.tv_sec = t.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)(t.tv_sec)*1000000000LL + t.tv_nsec;
}

static void *vsync_loop(void *param) ;

static bool created = false;

int vsync_enabled = 0;

static int hwc_eventControl(struct hwc_composer_device_1* dev, int dpy,
int event, int enable)
{
	    pthread_t vsync_thread;
	 hwc_context_t* ctx = (hwc_context_t*)(dev);
	ALOGD("hwc event con");
	if (event == HWC_EVENT_VSYNC) {
		ALOGD("vsync");
		vsync_enabled = enable;
		if (!created) {
			created = true;
			pthread_create(&vsync_thread, NULL, vsync_loop, (void*) ctx);
		}
//		tm += 10000000;
//		ctx->procs->vsync(ctx->procs, dpy, systemTime());
	}
	return 0;
}

static void *vsync_loop(void *param) {
		uint64_t cur_timestamp;
		 int dpy = HWC_DISPLAY_PRIMARY;
		 hwc_context_t * ctx = reinterpret_cast<hwc_context_t *>(param);
		do {
	     usleep(16666);
            cur_timestamp = systemTime();
	    if (vsync_enabled) {
	    	ALOGD("vsync to hal");
	         ctx->procs->vsync(ctx->procs, dpy, cur_timestamp);
		 }
		 }
	    while (true);
	return NULL;
}

static int hwc_blank(struct hwc_composer_device_1* dev, int dpy, int blank)
{
	ALOGD("blank");
	return 0;
}

static int hwc_query(struct hwc_composer_device_1* dev,
int param, int* value)
{
	ALOGD("query");
	
return 0;
}

static void hwc_registerProcs(struct hwc_composer_device_1* dev,
hwc_procs_t const* procs)
{
ALOGD("%s", __FUNCTION__);

 hwc_context_t* ctx = (hwc_context_t*)(dev);
if(!ctx) {
ALOGE("%s: Invalid context", __FUNCTION__);
return;
}
ctx->procs = procs;
// Now that we have the functions needed, kick off
// // the uevent & vsync threads
// init_uevent_thread(ctx);
// init_vsync_thread(ctx);

};

int hwc_getDisplayConfigs(struct hwc_composer_device_1* dev, int disp,
uint32_t* configs, size_t* numConfigs) {
ALOGD("%s", __FUNCTION__);
return 0;
}

int hwc_getDisplayAttributes(struct hwc_composer_device_1* dev, int disp,
uint32_t config, const uint32_t* attributes, int32_t* values) {
	ALOGD("%s", __FUNCTION__);
	return 0;
}

void hwc_dump(struct hwc_composer_device_1* dev, char *buff, int buff_len)
{
	ALOGD("HWC DUMP");
}

/*****************************************************************************/

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = -EINVAL;
    ALOGD("entering hwc...");
    if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
    	ALOGD("We're inside the composer");
        struct hwc_context_t *dev;
        dev = (hwc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));


        struct hw_module_t *gralloc;

	status = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t**)&gralloc);

        if (status < 0)
	        {
		                ALOGD("ERROR: Could not get gralloc");
				        }

	framebuffer_device_t *framebuffer = 0;
	status = framebuffer_open((hw_module_t *) gralloc, &framebuffer);

	if (status < 0)
	{
		ALOGD("ERROR: Could not get fb");
	}

	ALOGD("gralloc vendor is '%s'", gralloc->name);
	ALOGD("framebuffer is %p", framebuffer);



        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = HWC_DEVICE_API_VERSION_1_0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = hwc_device_close;

        dev->device.prepare = hwc_prepare;
        dev->device.set = hwc_set;


	dev->device.eventControl = hwc_eventControl;
	dev->device.blank = hwc_blank;
	dev->device.query = hwc_query;
	dev->device.registerProcs = hwc_registerProcs;
	dev->device.dump = hwc_dump;
	dev->device.getDisplayConfigs = hwc_getDisplayConfigs;
	dev->device.getDisplayAttributes = hwc_getDisplayAttributes;

	dev->framebuffer = framebuffer;
	dev->gralloc = gralloc;
	dev->fd = open("/dev/graphics/fb0", O_RDWR, 0);
	dev->mappedAddress = mmap(0, 800*480*4*2,
	 PROT_READ|PROT_WRITE, MAP_SHARED, dev->fd, 0);

        *device = &dev->device.common;
        status = 0;
    } else {
    	ALOGD("ups, something is wrong!");
    }
    ALOGD("returning %d", status);
    return status;
}
