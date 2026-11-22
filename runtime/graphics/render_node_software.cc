// andropkg: Canvas.drawRenderNode on a software canvas. Windows draw through Surface.lockCanvas
// (no HardwareRenderer), but apps build RenderNodes themselves and draw them into whatever
// canvas they get: Compose's GraphicsLayer (overscroll stretch, rememberGraphicsLayer), RenderNode
// API users. On Android that canvas is always a RecordingCanvas, which overrides drawRenderNode;
// the base Canvas method throws. ART marks Canvas.drawRenderNode native (class_linker.cc,
// kOverrides) and this plays the node with hwui's own RenderNodeDrawable on the CPU: the tree's
// staging properties and display lists are synced to their render copies (there is no render
// thread to do it in prepareTree), layers are rendered into raster surfaces, then the node is
// drawn into the canvas's SkCanvas.
#include <jni.h>
#include <log/log.h>

#include "RenderNode.h"
#include "SkiaCanvas.h"
#include "hwui/Canvas.h"
#include "pipeline/skia/RenderNodeDrawable.h"
#include "pipeline/skia/SkiaLayer.h"
#include "include/core/SkSurface.h"
#include <stdlib.h>

namespace android {
namespace uirenderer {

class AndropkgSoftwareRenderNode {
public:
    static void Sync(RenderNode* node, int depth) {
        if (depth > 256) return;  // a cycle: the app drew a node into itself
        MarkAndSweepRemoved observer(nullptr);
        node->syncProperties();
        if (node->mNeedsDisplayListSync) {
            node->mNeedsDisplayListSync = false;
            node->syncDisplayList(observer, nullptr);
        }
        if (auto& displayList = node->getDisplayList()) {
            displayList.updateChildren([depth](RenderNode* child) { Sync(child, depth + 1); });
        }
    }
    static SkCanvas* SkCanvasOf(SkiaCanvas* canvas) { return canvas->asSkCanvas(); }

    // Nodes that are layers (RenderEffect blur, Offscreen compositing, alpha over overlapping
    // content, LAYER_TYPE_HARDWARE) get a raster SkSurface and are rendered into it, innermost
    // first, as SkiaPipeline::renderLayers does on the render thread. RenderNodeDrawable then
    // composites them with their image filter, alpha and stretch.
    static void RenderLayers(RenderNode* node, int depth) {
        if (depth > 256) return;
        if (auto& displayList = node->getDisplayList()) {
            displayList.updateChildren([depth](RenderNode* child) { RenderLayers(child, depth + 1); });
        }
        const RenderProperties& props = node->properties();
        const int w = props.getWidth(), h = props.getHeight();
        if (props.effectiveLayerType() != LayerType::RenderLayer || !node->getDisplayList() || w <= 0 ||
            h <= 0 || static_cast<int64_t>(w) * h > (int64_t{1} << 24)) {
            if (node->hasLayer()) node->setLayerSurface(nullptr);
            return;
        }
        SkSurface* surface = node->getLayerSurface();
        if (surface == nullptr || surface->width() != w || surface->height() != h) {
            node->setLayerSurface(SkSurface::MakeRaster(SkImageInfo::MakeN32Premul(w, h)));
            surface = node->getLayerSurface();
            if (surface == nullptr) return;
        }
        SkCanvas* canvas = surface->getCanvas();
        canvas->clear(SK_ColorTRANSPARENT);
        skiapipeline::RenderNodeDrawable content(node, canvas, /*composeLayer=*/false);
        content.forceDraw(canvas);
        node->getSkiaLayer()->hasRenderedSinceRepaint = false;
    }

    // ANDROPKG_LOG_RENDERNODES: the tree a software canvas draws, now and then.
    static void Dump(RenderNode* node, int depth) {
        if (depth > 40) return;
        const RenderProperties& p = node->properties();
        ALOGI("rn %*s%s %dx%d at %d,%d t=%.0f,%.0f a=%.2f layer=%d filter=%d clip=%d dl=%d",
              depth * 2, "", node->getName(), p.getWidth(), p.getHeight(), p.getLeft(), p.getTop(),
              p.getTranslationX(), p.getTranslationY(), p.getAlpha(), static_cast<int>(p.effectiveLayerType()),
              p.layerProperties().getImageFilter() != nullptr, p.getClippingFlags(),
              static_cast<int>(static_cast<bool>(node->getDisplayList())));
        if (auto& displayList = node->getDisplayList()) {
            displayList.updateChildren([depth](RenderNode* child) { Dump(child, depth + 1); });
        }
    }
};

}  // namespace uirenderer

namespace {

jfieldID gCanvasNative;      // BaseCanvas.mNativeCanvasWrapper
jfieldID gRenderNodeNative;  // RenderNode.mNativeRenderNode

void Canvas_drawRenderNode(JNIEnv* env, jobject jcanvas, jobject jnode) {
    if (jnode == nullptr) return;
    auto* canvas = reinterpret_cast<Canvas*>(env->GetLongField(jcanvas, gCanvasNative));
    auto* node = reinterpret_cast<uirenderer::RenderNode*>(env->GetLongField(jnode, gRenderNodeNative));
    if (canvas == nullptr || node == nullptr) return;
    SkCanvas* sk = uirenderer::AndropkgSoftwareRenderNode::SkCanvasOf(static_cast<SkiaCanvas*>(canvas));
    if (sk == nullptr) return;
    uirenderer::AndropkgSoftwareRenderNode::Sync(node, 0);
    if (!node->getDisplayList()) return;  // nothing recorded yet
    uirenderer::AndropkgSoftwareRenderNode::RenderLayers(node, 0);
    static const bool dump = getenv("ANDROPKG_LOG_RENDERNODES") != nullptr;
    static int calls = 0;
    if (dump && calls++ % 300 == 0) uirenderer::AndropkgSoftwareRenderNode::Dump(node, 0);
    uirenderer::skiapipeline::RenderNodeDrawable drawable(node, sk);
    sk->drawDrawable(&drawable);
}

}  // namespace

int register_andropkg_software_render_node(JNIEnv* env) {
    jclass base = env->FindClass("android/graphics/BaseCanvas");
    gCanvasNative = env->GetFieldID(base, "mNativeCanvasWrapper", "J");
    jclass rn = env->FindClass("android/graphics/RenderNode");
    gRenderNodeNative = env->GetFieldID(rn, "mNativeRenderNode", "J");
    jclass canvas = env->FindClass("android/graphics/Canvas");
    static const JNINativeMethod kMethods[] = {
        {"drawRenderNode", "(Landroid/graphics/RenderNode;)V", reinterpret_cast<void*>(Canvas_drawRenderNode)},
    };
    if (gCanvasNative == nullptr || gRenderNodeNative == nullptr ||
        env->RegisterNatives(canvas, kMethods, 1) != JNI_OK) {
        env->ExceptionClear();
        ALOGW("andropkg: Canvas.drawRenderNode stays Java (no software RenderNode drawing)");
    }
    return 0;
}

}  // namespace android
