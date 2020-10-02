/*
 * Copyright (C) 2007 The Android Open Source Project
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

#define LOG_TAG "SurfaceControl"

#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <android/native_window.h>

#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/Log.h>
#include <utils/threads.h>

#include <binder/IPCThreadState.h>

#include <ui/DisplayInfo.h>
#include <ui/GraphicBuffer.h>
#include <ui/Rect.h>

#include <gui/BufferQueueCore.h>
#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>

namespace android {

// ============================================================================
//  SurfaceControl
// ============================================================================

SurfaceControl::SurfaceControl(const sp<SurfaceComposerClient>& client, const sp<IBinder>& handle,
                               const sp<IGraphicBufferProducer>& gbp, int32_t layerId,
                               uint32_t transform)
      : mClient(client),
        mHandle(handle),
        mGraphicBufferProducer(gbp),
        mLayerId(layerId),
        mTransformHint(transform) {}

SurfaceControl::SurfaceControl(const sp<SurfaceControl>& other) {
    mClient = other->mClient;
    mHandle = other->mHandle;
    mGraphicBufferProducer = other->mGraphicBufferProducer;
    mTransformHint = other->mTransformHint;
    mLayerId = other->mLayerId;
}

SurfaceControl::~SurfaceControl()
{
    // Trigger an IPC now, to make sure things
    // happen without delay, since these resources are quite heavy.
    mClient.clear();
    mHandle.clear();
    mGraphicBufferProducer.clear();
    IPCThreadState::self()->flushCommands();
}

void SurfaceControl::disconnect() {
    if (mGraphicBufferProducer != nullptr) {
        mGraphicBufferProducer->disconnect(
                BufferQueueCore::CURRENTLY_CONNECTED_API);
    }
}

bool SurfaceControl::isSameSurface(
        const sp<SurfaceControl>& lhs, const sp<SurfaceControl>& rhs)
{
    if (lhs == nullptr || rhs == nullptr)
        return false;
    return lhs->mHandle == rhs->mHandle;
}

status_t SurfaceControl::clearLayerFrameStats() const {
    status_t err = validate();
    if (err != NO_ERROR) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->clearLayerFrameStats(mHandle);
}

status_t SurfaceControl::getLayerFrameStats(FrameStats* outStats) const {
    status_t err = validate();
    if (err != NO_ERROR) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->getLayerFrameStats(mHandle, outStats);
}

status_t SurfaceControl::validate() const
{
    if (mHandle==nullptr || mClient==nullptr) {
        ALOGE("invalid handle (%p) or client (%p)",
                mHandle.get(), mClient.get());
        return NO_INIT;
    }
    return NO_ERROR;
}

status_t SurfaceControl::writeSurfaceToParcel(
        const sp<SurfaceControl>& control, Parcel* parcel)
{
    sp<IGraphicBufferProducer> bp;
    if (control != nullptr) {
        bp = control->mGraphicBufferProducer;
    }
    return parcel->writeStrongBinder(IInterface::asBinder(bp));
}

sp<Surface> SurfaceControl::generateSurfaceLocked() const
{
    // This surface is always consumed by SurfaceFlinger, so the
    // producerControlledByApp value doesn't matter; using false.
    mSurfaceData = new Surface(mGraphicBufferProducer, false);

    return mSurfaceData;
}

sp<Surface> SurfaceControl::getSurface() const
{
    Mutex::Autolock _l(mLock);
    if (mSurfaceData == nullptr) {
        return generateSurfaceLocked();
    }
    return mSurfaceData;
}

sp<Surface> SurfaceControl::createSurface() const
{
    Mutex::Autolock _l(mLock);
    return generateSurfaceLocked();
}

sp<IBinder> SurfaceControl::getHandle() const
{
    return mHandle;
}

int32_t SurfaceControl::getLayerId() const {
    return mLayerId;
}

sp<IGraphicBufferProducer> SurfaceControl::getIGraphicBufferProducer() const
{
    Mutex::Autolock _l(mLock);
    return mGraphicBufferProducer;
}

sp<SurfaceComposerClient> SurfaceControl::getClient() const
{
    return mClient;
}

uint32_t SurfaceControl::getTransformHint() const {
    Mutex::Autolock _l(mLock);
    return mTransformHint;
}

void SurfaceControl::setTransformHint(uint32_t hint) {
    Mutex::Autolock _l(mLock);
    mTransformHint = hint;
}

status_t SurfaceControl::writeToParcel(Parcel& parcel) {
    SAFE_PARCEL(parcel.writeStrongBinder, ISurfaceComposerClient::asBinder(mClient->getClient()));
    SAFE_PARCEL(parcel.writeStrongBinder, mHandle);
    SAFE_PARCEL(parcel.writeStrongBinder, IGraphicBufferProducer::asBinder(mGraphicBufferProducer));
    SAFE_PARCEL(parcel.writeInt32, mLayerId);
    SAFE_PARCEL(parcel.writeUint32, mTransformHint);

    return NO_ERROR;
}

status_t SurfaceControl::readFromParcel(const Parcel& parcel,
                                        sp<SurfaceControl>* outSurfaceControl) {
    sp<IBinder> client;
    sp<IBinder> handle;
    sp<IBinder> gbp;
    int32_t layerId;
    uint32_t transformHint;

    SAFE_PARCEL(parcel.readStrongBinder, &client);
    SAFE_PARCEL(parcel.readStrongBinder, &handle);
    SAFE_PARCEL(parcel.readNullableStrongBinder, &gbp);
    SAFE_PARCEL(parcel.readInt32, &layerId);
    SAFE_PARCEL(parcel.readUint32, &transformHint);

    // We aren't the original owner of the surface.
    *outSurfaceControl =
            new SurfaceControl(new SurfaceComposerClient(
                                       interface_cast<ISurfaceComposerClient>(client)),
                               handle.get(), interface_cast<IGraphicBufferProducer>(gbp), layerId,
                               transformHint);

    return NO_ERROR;
}

status_t SurfaceControl::readNullableFromParcel(const Parcel& parcel,
                                                sp<SurfaceControl>* outSurfaceControl) {
    bool isNotNull;
    SAFE_PARCEL(parcel.readBool, &isNotNull);
    if (isNotNull) {
        SAFE_PARCEL(SurfaceControl::readFromParcel, parcel, outSurfaceControl);
    }

    return NO_ERROR;
}

status_t SurfaceControl::writeNullableToParcel(Parcel& parcel,
                                               const sp<SurfaceControl>& surfaceControl) {
    auto isNotNull = surfaceControl != nullptr;
    SAFE_PARCEL(parcel.writeBool, isNotNull);
    if (isNotNull) {
        SAFE_PARCEL(surfaceControl->writeToParcel, parcel);
    }

    return NO_ERROR;
}

// ----------------------------------------------------------------------------
}; // namespace android
