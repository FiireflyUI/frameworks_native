/*
 * Copyright 2021 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "Planner"
// #define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <android-base/properties.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <compositionengine/impl/planner/CachedSet.h>
#include <math/HashCombine.h>
#include <renderengine/DisplaySettings.h>
#include <renderengine/RenderEngine.h>
#include <utils/Trace.h>

#include <utils/Trace.h>

namespace android::compositionengine::impl::planner {

const bool CachedSet::sDebugHighlighLayers =
        base::GetBoolProperty(std::string("debug.sf.layer_caching_highlight"), false);

std::string durationString(std::chrono::milliseconds duration) {
    using namespace std::chrono_literals;

    std::string result;

    if (duration >= 1h) {
        const auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
        base::StringAppendF(&result, "%d hr ", static_cast<int>(hours.count()));
        duration -= hours;
    }
    if (duration >= 1min) {
        const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
        base::StringAppendF(&result, "%d min ", static_cast<int>(minutes.count()));
        duration -= minutes;
    }
    base::StringAppendF(&result, "%.3f sec ", duration.count() / 1000.0f);

    return result;
}

CachedSet::Layer::Layer(const LayerState* state, std::chrono::steady_clock::time_point lastUpdate)
      : mState(state), mHash(state->getHash()), mLastUpdate(lastUpdate) {}

CachedSet::CachedSet(const LayerState* layer, std::chrono::steady_clock::time_point lastUpdate)
      : mFingerprint(layer->getHash()), mLastUpdate(lastUpdate) {
    addLayer(layer, lastUpdate);
}

CachedSet::CachedSet(Layer layer)
      : mFingerprint(layer.getHash()),
        mLastUpdate(layer.getLastUpdate()),
        mBounds(layer.getDisplayFrame()),
        mVisibleRegion(layer.getVisibleRegion()) {
    mLayers.emplace_back(std::move(layer));
}

void CachedSet::addLayer(const LayerState* layer,
                         std::chrono::steady_clock::time_point lastUpdate) {
    mLayers.emplace_back(layer, lastUpdate);

    Region boundingRegion;
    boundingRegion.orSelf(mBounds);
    boundingRegion.orSelf(layer->getDisplayFrame());
    mBounds = boundingRegion.getBounds();
    mVisibleRegion.orSelf(layer->getVisibleRegion());
}

NonBufferHash CachedSet::getNonBufferHash() const {
    if (mLayers.size() == 1) {
        return mFingerprint;
    }

    // TODO(b/182614524): We sometimes match this with LayerState hashes. Determine if that is
    // necessary (and therefore we need to match implementations).
    size_t hash = 0;
    android::hashCombineSingle(hash, mBounds);
    android::hashCombineSingle(hash, mOutputDataspace);
    android::hashCombineSingle(hash, mOrientation);
    return hash;
}

size_t CachedSet::getComponentDisplayCost() const {
    size_t displayCost = 0;

    for (const Layer& layer : mLayers) {
        displayCost += static_cast<size_t>(layer.getDisplayFrame().width() *
                                           layer.getDisplayFrame().height());
    }

    return displayCost;
}

size_t CachedSet::getCreationCost() const {
    if (mLayers.size() == 1) {
        return 0;
    }

    // Reads
    size_t creationCost = getComponentDisplayCost();

    // Write - assumes that the output buffer only gets written once per pixel
    creationCost += static_cast<size_t>(mBounds.width() * mBounds.height());

    return creationCost;
}

size_t CachedSet::getDisplayCost() const {
    return static_cast<size_t>(mBounds.width() * mBounds.height());
}

bool CachedSet::hasBufferUpdate() const {
    for (const Layer& layer : mLayers) {
        if (layer.getFramesSinceBufferUpdate() == 0) {
            return true;
        }
    }
    return false;
}

bool CachedSet::hasReadyBuffer() const {
    return mTexture && mDrawFence->getStatus() == Fence::Status::Signaled;
}

std::vector<CachedSet> CachedSet::decompose() const {
    std::vector<CachedSet> layers;

    std::transform(mLayers.begin(), mLayers.end(), std::back_inserter(layers),
                   [](Layer layer) { return CachedSet(std::move(layer)); });

    return layers;
}

void CachedSet::updateAge(std::chrono::steady_clock::time_point now) {
    LOG_ALWAYS_FATAL_IF(mLayers.size() > 1, "[%s] This should only be called on single-layer sets",
                        __func__);

    if (mLayers[0].getFramesSinceBufferUpdate() == 0) {
        mLastUpdate = now;
        mAge = 0;
    }
}

void CachedSet::render(renderengine::RenderEngine& renderEngine, TexturePool& texturePool,
                       const OutputCompositionState& outputState) {
    ATRACE_CALL();
    const Rect& viewport = outputState.layerStackSpace.content;
    const ui::Dataspace& outputDataspace = outputState.dataspace;
    const ui::Transform::RotationFlags orientation =
            ui::Transform::toRotationFlags(outputState.framebufferSpace.orientation);

    renderengine::DisplaySettings displaySettings{
            .physicalDisplay = outputState.framebufferSpace.content,
            .clip = viewport,
            .outputDataspace = outputDataspace,
            .orientation = orientation,
    };

    Region clearRegion = Region::INVALID_REGION;
    LayerFE::ClientCompositionTargetSettings targetSettings{
            .clip = Region(viewport),
            .needsFiltering = false,
            .isSecure = true,
            .supportsProtectedContent = false,
            .clearRegion = clearRegion,
            .viewport = viewport,
            .dataspace = outputDataspace,
            .realContentIsVisible = true,
            .clearContent = false,
            .blurSetting = LayerFE::ClientCompositionTargetSettings::BlurSetting::Enabled,
    };

    std::vector<renderengine::LayerSettings> layerSettings;
    renderengine::LayerSettings highlight;
    for (const auto& layer : mLayers) {
        const auto clientCompositionList =
                layer.getState()->getOutputLayer()->getLayerFE().prepareClientCompositionList(
                        targetSettings);
        layerSettings.insert(layerSettings.end(), clientCompositionList.cbegin(),
                             clientCompositionList.cend());
    }

    std::vector<const renderengine::LayerSettings*> layerSettingsPointers;
    std::transform(layerSettings.cbegin(), layerSettings.cend(),
                   std::back_inserter(layerSettingsPointers),
                   [](const renderengine::LayerSettings& settings) { return &settings; });

    renderengine::LayerSettings blurLayerSettings;
    if (mBlurLayer) {
        auto blurSettings = targetSettings;
        blurSettings.blurSetting =
                LayerFE::ClientCompositionTargetSettings::BlurSetting::BackgroundBlurOnly;
        auto clientCompositionList =
                mBlurLayer->getOutputLayer()->getLayerFE().prepareClientCompositionList(
                        blurSettings);
        blurLayerSettings = clientCompositionList.back();
        // This mimics Layer::prepareClearClientComposition
        blurLayerSettings.skipContentDraw = true;
        blurLayerSettings.name = std::string("blur layer");
        // Clear out the shadow settings
        blurLayerSettings.shadow = {};
        layerSettingsPointers.push_back(&blurLayerSettings);
    }

    renderengine::LayerSettings holePunchSettings;
    if (mHolePunchLayer) {
        auto clientCompositionList =
                mHolePunchLayer->getOutputLayer()->getLayerFE().prepareClientCompositionList(
                        targetSettings);
        // Assume that the final layer contains the buffer that we want to
        // replace with a hole punch.
        holePunchSettings = clientCompositionList.back();
        // This mimics Layer::prepareClearClientComposition
        holePunchSettings.source.buffer.buffer = nullptr;
        holePunchSettings.source.solidColor = half3(0.0f, 0.0f, 0.0f);
        holePunchSettings.disableBlending = true;
        holePunchSettings.alpha = 0.0f;
        holePunchSettings.name = std::string("hole punch layer");
        layerSettingsPointers.push_back(&holePunchSettings);
    }

    if (sDebugHighlighLayers) {
        highlight = {
                .geometry =
                        renderengine::Geometry{
                                .boundaries = FloatRect(0.0f, 0.0f,
                                                        static_cast<float>(mBounds.getWidth()),
                                                        static_cast<float>(mBounds.getHeight())),
                        },
                .source =
                        renderengine::PixelSource{
                                .solidColor = half3(0.25f, 0.0f, 0.5f),
                        },
                .alpha = half(0.05f),
        };

        layerSettingsPointers.emplace_back(&highlight);
    }

    auto texture = texturePool.borrowTexture();
    LOG_ALWAYS_FATAL_IF(texture->get()->getBuffer()->initCheck() != OK);

    base::unique_fd bufferFence;
    if (texture->getReadyFence()) {
        // Bail out if the buffer is not ready, because there is some pending GPU work left.
        if (texture->getReadyFence()->getStatus() != Fence::Status::Signaled) {
            return;
        }
        bufferFence.reset(texture->getReadyFence()->dup());
    }

    base::unique_fd drawFence;
    status_t result =
            renderEngine.drawLayers(displaySettings, layerSettingsPointers, texture->get(), false,
                                    std::move(bufferFence), &drawFence);

    if (result == NO_ERROR) {
        mDrawFence = new Fence(drawFence.release());
        mOutputSpace = outputState.framebufferSpace;
        mTexture = texture;
        mTexture->setReadyFence(mDrawFence);
        mOutputSpace.orientation = outputState.framebufferSpace.orientation;
        mOutputDataspace = outputDataspace;
        mOrientation = orientation;
        mSkipCount = 0;
    } else {
        mTexture.reset();
    }
}

bool CachedSet::requiresHolePunch() const {
    // In order for the hole punch to be beneficial, the layer must be updating
    // regularly, meaning  it should not have been merged with other layers.
    if (getLayerCount() != 1) {
        return false;
    }

    // There is no benefit to a hole punch unless the layer has a buffer.
    if (!mLayers[0].getBuffer()) {
        return false;
    }

    // Do not use a hole punch with an HDR layer; this should be done in client
    // composition to properly mix HDR with SDR.
    if (hasHdrLayers()) {
        return false;
    }

    const auto& layerFE = mLayers[0].getState()->getOutputLayer()->getLayerFE();
    if (layerFE.getCompositionState()->forceClientComposition) {
        return false;
    }

    return layerFE.hasRoundedCorners();
}

bool CachedSet::hasBlurBehind() const {
    return std::any_of(mLayers.cbegin(), mLayers.cend(),
                       [](const Layer& layer) { return layer.getState()->hasBlurBehind(); });
}

namespace {
bool contains(const Rect& outer, const Rect& inner) {
    return outer.left <= inner.left && outer.right >= inner.right && outer.top <= inner.top &&
            outer.bottom >= inner.bottom;
}
}; // namespace

void CachedSet::addHolePunchLayerIfFeasible(const CachedSet& holePunchLayer, bool isFirstLayer) {
    // Verify that this CachedSet is opaque where the hole punch layer
    // will draw.
    const Rect& holePunchBounds = holePunchLayer.getBounds();
    for (const auto& layer : mLayers) {
        // The first layer is considered opaque because nothing is behind it.
        // Note that isOpaque is always false for a layer with rounded
        // corners, even if the interior is opaque. In theory, such a layer
        // could be used for a hole punch, but this is unlikely to happen in
        // practice.
        const auto* outputLayer = layer.getState()->getOutputLayer();
        if (contains(outputLayer->getState().displayFrame, holePunchBounds) &&
            (isFirstLayer || outputLayer->getLayerFE().getCompositionState()->isOpaque)) {
            mHolePunchLayer = holePunchLayer.getFirstLayer().getState();
            return;
        }
    }
}

void CachedSet::addBackgroundBlurLayer(const CachedSet& blurLayer) {
    mBlurLayer = blurLayer.getFirstLayer().getState();
}

compositionengine::OutputLayer* CachedSet::getHolePunchLayer() const {
    return mHolePunchLayer ? mHolePunchLayer->getOutputLayer() : nullptr;
}

compositionengine::OutputLayer* CachedSet::getBlurLayer() const {
    return mBlurLayer ? mBlurLayer->getOutputLayer() : nullptr;
}

bool CachedSet::hasHdrLayers() const {
    return std::any_of(mLayers.cbegin(), mLayers.cend(),
                       [](const Layer& layer) { return layer.getState()->isHdr(); });
}

bool CachedSet::hasProtectedLayers() const {
    return std::any_of(mLayers.cbegin(), mLayers.cend(),
                       [](const Layer& layer) { return layer.getState()->isProtected(); });
}

void CachedSet::dump(std::string& result) const {
    const auto now = std::chrono::steady_clock::now();

    const auto lastUpdate =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastUpdate);
    base::StringAppendF(&result, "  + Fingerprint %016zx, last update %sago, age %zd\n",
                        mFingerprint, durationString(lastUpdate).c_str(), mAge);
    {
        const auto b = mTexture ? mTexture->get()->getBuffer().get() : nullptr;
        base::StringAppendF(&result, "    Override buffer: %p\n", b);
    }
    base::StringAppendF(&result, "    HolePunchLayer: %p\n", mHolePunchLayer);

    if (mLayers.size() == 1) {
        base::StringAppendF(&result, "    Layer [%s]\n", mLayers[0].getName().c_str());
        base::StringAppendF(&result, "    Buffer %p", mLayers[0].getBuffer().get());
    } else {
        result.append("    Cached set of:");
        for (const Layer& layer : mLayers) {
            base::StringAppendF(&result, "\n      Layer [%s]", layer.getName().c_str());
        }
    }

    base::StringAppendF(&result, "\n    Creation cost: %zd", getCreationCost());
    base::StringAppendF(&result, "\n    Display cost: %zd\n", getDisplayCost());
}

} // namespace android::compositionengine::impl::planner