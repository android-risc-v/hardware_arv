/*
 * Copyright 2020 Android-RPi Project
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

#define LOG_TAG "mapper@2.0-Mapper"
//#define LOG_NDEBUG 0
#include <android-base/logging.h>
#include <utils/Log.h>
#include <inttypes.h>
#include <mapper-passthrough/2.0/GrallocBufferDescriptor.h>
#include <hardware/gralloc1.h>

#include "gbm_module.h"
#include "Mapper.h"

namespace android {
namespace hardware {
namespace graphics {
namespace mapper {
namespace V2_0 {
namespace implementation {

Mapper::Mapper() {
    ALOGV("Constructing");
    mModule = new gbm_module_t;
    mModule->gbm = nullptr;
    int error = gbm_mod_init(mModule);
    if (error) {
        ALOGE("Failed Mapper() %d", error);
    }
}

Mapper::~Mapper() {
	ALOGV("Destructing");
    if (mModule != nullptr) {
        gbm_mod_deinit(mModule);
        delete mModule;
    }
}


static uint64_t getValidBufferUsageMask() {
	return BufferUsage::CPU_READ_MASK | BufferUsage::CPU_WRITE_MASK | BufferUsage::GPU_TEXTURE |
		   BufferUsage::GPU_RENDER_TARGET | BufferUsage::COMPOSER_OVERLAY |
		   BufferUsage::COMPOSER_CLIENT_TARGET | BufferUsage::PROTECTED |
		   BufferUsage::COMPOSER_CURSOR | BufferUsage::VIDEO_ENCODER |
		   BufferUsage::CAMERA_OUTPUT | BufferUsage::CAMERA_INPUT | BufferUsage::RENDERSCRIPT |
		   BufferUsage::VIDEO_DECODER | BufferUsage::SENSOR_DIRECT_DATA |
		   BufferUsage::GPU_DATA_BUFFER | BufferUsage::VENDOR_MASK |
		   BufferUsage::VENDOR_MASK_HI;
}

Return<void> Mapper::createDescriptor(const IMapper::BufferDescriptorInfo& descriptorInfo,
        createDescriptor_cb hidl_cb) {
	Error error = Error::NONE;
    if (!descriptorInfo.width || !descriptorInfo.height || !descriptorInfo.layerCount) {
        error = Error::BAD_VALUE;
    } else if (descriptorInfo.layerCount != 1) {
        error = Error::UNSUPPORTED;
    } else if (descriptorInfo.format == static_cast<PixelFormat>(0)) {
        error = Error::BAD_VALUE;
    }

    BufferDescriptor descriptor;
    if (error == Error::NONE) {
        const uint64_t validUsageBits = getValidBufferUsageMask();
        if (descriptorInfo.usage & ~validUsageBits) {
            ALOGW("buffer descriptor with invalid usage bits 0x%" PRIx64,
                    descriptorInfo.usage & ~validUsageBits);
        }
        descriptor = grallocEncodeBufferDescriptor(descriptorInfo);
	}
    hidl_cb(error, descriptor);
    return Void();
}

Return<void> Mapper::importBuffer(const hidl_handle& rawHandle,
		IMapper::importBuffer_cb hidl_cb) {
    if (!rawHandle.getNativeHandle()) {
        hidl_cb(Error::BAD_BUFFER, nullptr);
        return Void();
    }
    Error error = Error::NONE;
    native_handle_t* bufferHandle = native_handle_clone(rawHandle.getNativeHandle());
    if (!bufferHandle) {
        error = Error::NO_RESOURCES;
    }

    ALOGV("register(%p)", bufferHandle);
    int result = gbm_mod_register(mModule, bufferHandle);
    if (result != 0) {
        ALOGE("gbm register failed: %d", result);
        native_handle_close(bufferHandle);
        native_handle_delete(bufferHandle);
        bufferHandle = nullptr;
        error = Error::NO_RESOURCES;
    }

    hidl_cb(error, bufferHandle);
    return Void();
}

Return<Error> Mapper::freeBuffer(void* buffer) {
    Error error = Error::NONE;
    native_handle_t* bufferHandle = static_cast<native_handle_t*>(buffer);
    if (!bufferHandle) {
        error = Error::BAD_BUFFER;
    }
    if (error == Error::NONE) {
        ALOGV("unregister(%p)", bufferHandle);
        int result = gbm_mod_unregister(mModule, bufferHandle);
        if (result != 0) {
            ALOGE("gbm unregister failed: %d", result);
            error = Error::UNSUPPORTED;
        } else {
            native_handle_close(bufferHandle);
            native_handle_delete(bufferHandle);
        }
    }
    return error;
}


static Error getFenceFd(const hidl_handle& fenceHandle, base::unique_fd* outFenceFd) {
    auto handle = fenceHandle.getNativeHandle();
    if (handle && handle->numFds > 1) {
        ALOGE("invalid fence handle with %d fds", handle->numFds);
        return Error::BAD_VALUE;
    }
    int fenceFd = (handle && handle->numFds == 1) ? handle->data[0] : -1;
    if (fenceFd >= 0) {
        fenceFd = dup(fenceFd);
        if (fenceFd < 0) {
            return Error::NO_RESOURCES;
        }
    }
    outFenceFd->reset(fenceFd);
    return Error::NONE;
}

Return<void> Mapper::lock(void* buffer, uint64_t cpuUsage, const IMapper::Rect& accessRegion,
                  const hidl_handle& acquireFence, IMapper::lock_cb hidl_cb) {
    const native_handle_t* bufferHandle = static_cast<const native_handle_t*>(buffer);
    if (!bufferHandle) {
        hidl_cb(Error::BAD_BUFFER, nullptr);
        return Void();
    }

    const auto pUsage = static_cast<gralloc1_producer_usage_t>(cpuUsage);
    const auto cUsage = static_cast<gralloc1_consumer_usage_t>(cpuUsage
            & ~static_cast<uint64_t>(BufferUsage::CPU_WRITE_MASK));
    const auto usage = static_cast<int32_t>(pUsage | cUsage);

    const auto accessRect = gralloc1_rect_t{accessRegion.left, accessRegion.top,
                 accessRegion.width, accessRegion.height};

    base::unique_fd fenceFd;
    Error error = getFenceFd(acquireFence, &fenceFd);
    if (error != Error::NONE) {
        hidl_cb(error, nullptr);
        return Void();
    }
    sp<Fence> aFence{new Fence(fenceFd.release())};
    aFence->waitForever("Mapper::lock");

    void* data = nullptr;
    int result = gbm_mod_lock(mModule, bufferHandle, usage, accessRect.left, accessRect.top,
            accessRect.width, accessRect.height, &data);

    if (result != 0) {
    	ALOGE("gbm_lock() returned %d", result);
        hidl_cb(Error::UNSUPPORTED, nullptr);
    } else {
        hidl_cb(error, data);
    }
    return Void();
}


Return<void> Mapper::lockYCbCr(void* buffer, uint64_t cpuUsage, const IMapper::Rect& accessRegion,
                  const hidl_handle& acquireFence, IMapper::lockYCbCr_cb hidl_cb) {
    const native_handle_t* bufferHandle = static_cast<const native_handle_t*>(buffer);
    if (!bufferHandle) {
        hidl_cb(Error::BAD_BUFFER, YCbCrLayout{});
        return Void();
    }

    const auto pUsage = static_cast<gralloc1_producer_usage_t>(cpuUsage);
    const auto cUsage = static_cast<gralloc1_consumer_usage_t>(cpuUsage
            & ~static_cast<uint64_t>(BufferUsage::CPU_WRITE_MASK));
    const auto usage = static_cast<int32_t>(pUsage | cUsage);

    const auto accessRect = gralloc1_rect_t{accessRegion.left, accessRegion.top,
                 accessRegion.width, accessRegion.height};

    base::unique_fd fenceFd;
    Error error = getFenceFd(acquireFence, &fenceFd);
    if (error != Error::NONE) {
        hidl_cb(error, YCbCrLayout{});
        return Void();
    }
    sp<Fence> aFence{new Fence(fenceFd.release())};
    aFence->waitForever("Mapper::lockYCbCr");

    android_ycbcr ycbcr = {};
    int result = gbm_mod_lock_ycbcr(mModule, bufferHandle, usage, accessRect.left, accessRect.top,
            accessRect.width, accessRect.height, &ycbcr);

    if (result != 0) {
    	ALOGE("gbm_mod_lock_ycbcr() returned %d", result);
        hidl_cb(Error::UNSUPPORTED, YCbCrLayout{});
    } else {
        YCbCrLayout layout{};
        layout.y = ycbcr.y;
        layout.cb = ycbcr.cb;
        layout.cr = ycbcr.cr;
        layout.yStride = ycbcr.ystride;
        layout.cStride = ycbcr.cstride;
        layout.chromaStep = ycbcr.chroma_step;
        hidl_cb(error, layout);
    }
    return Void();
}


static hidl_handle getFenceHandle(const base::unique_fd& fenceFd, char* handleStorage) {
    native_handle_t* handle = nullptr;
    if (fenceFd >= 0) {
        handle = native_handle_init(handleStorage, 1, 0);
        handle->data[0] = fenceFd;
    }
    return hidl_handle(handle);
}

Return<void> Mapper::unlock(void* buffer, IMapper::unlock_cb hidl_cb) {
    const native_handle_t* bufferHandle = static_cast<const native_handle_t*>(buffer);
    if (!bufferHandle) {
        hidl_cb(Error::BAD_BUFFER, nullptr);
        return Void();
    }

    int result = gbm_mod_unlock(mModule, bufferHandle);
	if (result != 0) {
		ALOGE("gralloc0 unlock failed: %d", result);
        hidl_cb(Error::UNSUPPORTED, nullptr);
        return Void();
	}

    base::unique_fd fenceFd;
    fenceFd.reset((Fence::NO_FENCE)->dup());
    NATIVE_HANDLE_DECLARE_STORAGE(fenceStorage, 1, 0);
    hidl_cb(Error::NONE, getFenceHandle(fenceFd, fenceStorage));
    return Void();
}


IMapper* HIDL_FETCH_IMapper(const char* /* name */) {
    return new Mapper();
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace mapper
}  // namespace graphics
}  // namespace hardware
}  // namespace android
