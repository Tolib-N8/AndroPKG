// andropkg: Canvas.drawRenderNode on a software canvas. Windows draw through Surface.lockCanvas
// (no HardwareRenderer), but apps build RenderNodes themselves and draw them into whatever
// canvas they get: Compose's GraphicsLayer (overscroll stretch, rememberGraphicsLayer), RenderNode
// API users. On Android that canvas is always a RecordingCanvas, which overrides drawRenderNode;
// the base Canvas method throws. ART marks Canvas.drawRenderNode native (class_linker.cc,
// kOverrides) and this plays the node with hwui's own RenderNodeDrawable on the CPU: the tree's
// staging properties and display lists are synced to their render copies (there is no render
// thread to do it in prepareTree), then drawn into the canvas's SkCanvas. Without layer
// surfaces, hardware layers draw their content directly, and stretch effects are not applied.
#include <jni.h>
#include <log/log.h>

#include "RenderNode.h"
#include "SkiaCanvas.h"
#include "hwui/Canvas.h"
#include "pipeline/skia/RenderNodeDrawable.h"

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
