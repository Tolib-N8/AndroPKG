package dev.andropkg.system.webview;

import java.util.HashMap;
import java.util.Map;

/**
 * android.webkit.WebSettings for andropkg's WebView: settings are remembered and read back
 * (with Chromium's defaults), they change nothing else. Generated from the API 37 abstract
 * methods of WebSettings.
 */
@SuppressWarnings({"deprecation", "unchecked"})
final class StubWebSettings extends android.webkit.WebSettings {
    private final Map<String, Object> values = new HashMap<>();

    @Override public boolean enableSmoothTransition() { Object v = values.get("EnableSmoothTransition"); return v != null ? (Boolean) v : false; }
    @Override public boolean getAllowContentAccess() { Object v = values.get("AllowContentAccess"); return v != null ? (Boolean) v : true; }
    @Override public boolean getAllowFileAccess() { Object v = values.get("AllowFileAccess"); return v != null ? (Boolean) v : false; }
    @Override public boolean getAllowFileAccessFromFileURLs() { Object v = values.get("AllowFileAccessFromFileURLs"); return v != null ? (Boolean) v : false; }
    @Override public boolean getAllowUniversalAccessFromFileURLs() { Object v = values.get("AllowUniversalAccessFromFileURLs"); return v != null ? (Boolean) v : false; }
    @Override public boolean getBlockNetworkImage() { Object v = values.get("BlockNetworkImage"); return v != null ? (Boolean) v : false; }
    @Override public boolean getBlockNetworkLoads() { Object v = values.get("BlockNetworkLoads"); return v != null ? (Boolean) v : false; }
    @Override public boolean getBuiltInZoomControls() { Object v = values.get("BuiltInZoomControls"); return v != null ? (Boolean) v : false; }
    @Override public int getCacheMode() { Object v = values.get("CacheMode"); return v != null ? (Integer) v : LOAD_DEFAULT; }
    @Override public java.lang.String getCursiveFontFamily() { Object v = values.get("CursiveFontFamily"); return v != null ? (java.lang.String) v : "cursive"; }
    @Override public boolean getDatabaseEnabled() { Object v = values.get("DatabaseEnabled"); return v != null ? (Boolean) v : false; }
    @Override public java.lang.String getDatabasePath() { Object v = values.get("DatabasePath"); return v != null ? (java.lang.String) v : null; }
    @Override public int getDefaultFixedFontSize() { Object v = values.get("DefaultFixedFontSize"); return v != null ? (Integer) v : 13; }
    @Override public int getDefaultFontSize() { Object v = values.get("DefaultFontSize"); return v != null ? (Integer) v : 16; }
    @Override public java.lang.String getDefaultTextEncodingName() { Object v = values.get("DefaultTextEncodingName"); return v != null ? (java.lang.String) v : "UTF-8"; }
    @Override public android.webkit.WebSettings.ZoomDensity getDefaultZoom() { Object v = values.get("DefaultZoom"); return v != null ? (android.webkit.WebSettings.ZoomDensity) v : null; }
    @Override public int getDisabledActionModeMenuItems() { Object v = values.get("DisabledActionModeMenuItems"); return v != null ? (Integer) v : 0; }
    @Override public boolean getDisplayZoomControls() { Object v = values.get("DisplayZoomControls"); return v != null ? (Boolean) v : true; }
    @Override public boolean getDomStorageEnabled() { Object v = values.get("DomStorageEnabled"); return v != null ? (Boolean) v : false; }
    @Override public java.lang.String getFantasyFontFamily() { Object v = values.get("FantasyFontFamily"); return v != null ? (java.lang.String) v : "fantasy"; }
    @Override public java.lang.String getFixedFontFamily() { Object v = values.get("FixedFontFamily"); return v != null ? (java.lang.String) v : "monospace"; }
    @Override public boolean getJavaScriptCanOpenWindowsAutomatically() { Object v = values.get("JavaScriptCanOpenWindowsAutomatically"); return v != null ? (Boolean) v : false; }
    @Override public boolean getJavaScriptEnabled() { Object v = values.get("JavaScriptEnabled"); return v != null ? (Boolean) v : false; }
    @Override public android.webkit.WebSettings.LayoutAlgorithm getLayoutAlgorithm() { Object v = values.get("LayoutAlgorithm"); return v != null ? (android.webkit.WebSettings.LayoutAlgorithm) v : LayoutAlgorithm.NARROW_COLUMNS; }
    @Override public boolean getLightTouchEnabled() { Object v = values.get("LightTouchEnabled"); return v != null ? (Boolean) v : false; }
    @Override public boolean getLoadWithOverviewMode() { Object v = values.get("LoadWithOverviewMode"); return v != null ? (Boolean) v : false; }
    @Override public boolean getLoadsImagesAutomatically() { Object v = values.get("LoadsImagesAutomatically"); return v != null ? (Boolean) v : true; }
    @Override public boolean getMediaPlaybackRequiresUserGesture() { Object v = values.get("MediaPlaybackRequiresUserGesture"); return v != null ? (Boolean) v : true; }
    @Override public int getMinimumFontSize() { Object v = values.get("MinimumFontSize"); return v != null ? (Integer) v : 8; }
    @Override public int getMinimumLogicalFontSize() { Object v = values.get("MinimumLogicalFontSize"); return v != null ? (Integer) v : 8; }
    @Override public int getMixedContentMode() { Object v = values.get("MixedContentMode"); return v != null ? (Integer) v : MIXED_CONTENT_NEVER_ALLOW; }
    @Override public boolean getOffscreenPreRaster() { Object v = values.get("OffscreenPreRaster"); return v != null ? (Boolean) v : false; }
    @Override public android.webkit.WebSettings.PluginState getPluginState() { Object v = values.get("PluginState"); return v != null ? (android.webkit.WebSettings.PluginState) v : PluginState.OFF; }
    @Override public boolean getSafeBrowsingEnabled() { Object v = values.get("SafeBrowsingEnabled"); return v != null ? (Boolean) v : true; }
    @Override public java.lang.String getSansSerifFontFamily() { Object v = values.get("SansSerifFontFamily"); return v != null ? (java.lang.String) v : "sans-serif"; }
    @Override public boolean getSaveFormData() { Object v = values.get("SaveFormData"); return v != null ? (Boolean) v : false; }
    @Override public boolean getSavePassword() { Object v = values.get("SavePassword"); return v != null ? (Boolean) v : false; }
    @Override public java.lang.String getSerifFontFamily() { Object v = values.get("SerifFontFamily"); return v != null ? (java.lang.String) v : "serif"; }
    @Override public java.lang.String getStandardFontFamily() { Object v = values.get("StandardFontFamily"); return v != null ? (java.lang.String) v : "sans-serif"; }
    @Override public int getTextZoom() { Object v = values.get("TextZoom"); return v != null ? (Integer) v : 100; }
    @Override public boolean getUseWideViewPort() { Object v = values.get("UseWideViewPort"); return v != null ? (Boolean) v : false; }
    @Override public java.lang.String getUserAgentString() { Object v = values.get("UserAgentString"); return v != null ? (java.lang.String) v : TextWebView.USER_AGENT; }
    @Override public void setAllowContentAccess(boolean a0) { values.put("AllowContentAccess", a0); }
    @Override public void setAllowFileAccess(boolean a0) { values.put("AllowFileAccess", a0); }
    @Override public void setAllowFileAccessFromFileURLs(boolean a0) { values.put("AllowFileAccessFromFileURLs", a0); }
    @Override public void setAllowUniversalAccessFromFileURLs(boolean a0) { values.put("AllowUniversalAccessFromFileURLs", a0); }
    @Override public void setBlockNetworkImage(boolean a0) { values.put("BlockNetworkImage", a0); }
    @Override public void setBlockNetworkLoads(boolean a0) { values.put("BlockNetworkLoads", a0); }
    @Override public void setBuiltInZoomControls(boolean a0) { values.put("BuiltInZoomControls", a0); }
    @Override public void setCacheMode(int a0) { values.put("CacheMode", a0); }
    @Override public void setCursiveFontFamily(java.lang.String a0) { values.put("CursiveFontFamily", a0); }
    @Override public void setDatabaseEnabled(boolean a0) { values.put("DatabaseEnabled", a0); }
    @Override public void setDatabasePath(java.lang.String a0) { values.put("DatabasePath", a0); }
    @Override public void setDefaultFixedFontSize(int a0) { values.put("DefaultFixedFontSize", a0); }
    @Override public void setDefaultFontSize(int a0) { values.put("DefaultFontSize", a0); }
    @Override public void setDefaultTextEncodingName(java.lang.String a0) { values.put("DefaultTextEncodingName", a0); }
    @Override public void setDefaultZoom(android.webkit.WebSettings.ZoomDensity a0) { values.put("DefaultZoom", a0); }
    @Override public void setDisabledActionModeMenuItems(int a0) { values.put("DisabledActionModeMenuItems", a0); }
    @Override public void setDisplayZoomControls(boolean a0) { values.put("DisplayZoomControls", a0); }
    @Override public void setDomStorageEnabled(boolean a0) { values.put("DomStorageEnabled", a0); }
    @Override public void setEnableSmoothTransition(boolean a0) { values.put("EnableSmoothTransition", a0); }
    @Override public void setFantasyFontFamily(java.lang.String a0) { values.put("FantasyFontFamily", a0); }
    @Override public void setFixedFontFamily(java.lang.String a0) { values.put("FixedFontFamily", a0); }
    @Override public void setGeolocationDatabasePath(java.lang.String a0) { values.put("GeolocationDatabasePath", a0); }
    @Override public void setGeolocationEnabled(boolean a0) { values.put("GeolocationEnabled", a0); }
    @Override public void setJavaScriptCanOpenWindowsAutomatically(boolean a0) { values.put("JavaScriptCanOpenWindowsAutomatically", a0); }
    @Override public void setJavaScriptEnabled(boolean a0) { values.put("JavaScriptEnabled", a0); }
    @Override public void setLayoutAlgorithm(android.webkit.WebSettings.LayoutAlgorithm a0) { values.put("LayoutAlgorithm", a0); }
    @Override public void setLightTouchEnabled(boolean a0) { values.put("LightTouchEnabled", a0); }
    @Override public void setLoadWithOverviewMode(boolean a0) { values.put("LoadWithOverviewMode", a0); }
    @Override public void setLoadsImagesAutomatically(boolean a0) { values.put("LoadsImagesAutomatically", a0); }
    @Override public void setMediaPlaybackRequiresUserGesture(boolean a0) { values.put("MediaPlaybackRequiresUserGesture", a0); }
    @Override public void setMinimumFontSize(int a0) { values.put("MinimumFontSize", a0); }
    @Override public void setMinimumLogicalFontSize(int a0) { values.put("MinimumLogicalFontSize", a0); }
    @Override public void setMixedContentMode(int a0) { values.put("MixedContentMode", a0); }
    @Override public void setNeedInitialFocus(boolean a0) { values.put("NeedInitialFocus", a0); }
    @Override public void setOffscreenPreRaster(boolean a0) { values.put("OffscreenPreRaster", a0); }
    @Override public void setPluginState(android.webkit.WebSettings.PluginState a0) { values.put("PluginState", a0); }
    @Override public void setRenderPriority(android.webkit.WebSettings.RenderPriority a0) { values.put("RenderPriority", a0); }
    @Override public void setSafeBrowsingEnabled(boolean a0) { values.put("SafeBrowsingEnabled", a0); }
    @Override public void setSansSerifFontFamily(java.lang.String a0) { values.put("SansSerifFontFamily", a0); }
    @Override public void setSaveFormData(boolean a0) { values.put("SaveFormData", a0); }
    @Override public void setSavePassword(boolean a0) { values.put("SavePassword", a0); }
    @Override public void setSerifFontFamily(java.lang.String a0) { values.put("SerifFontFamily", a0); }
    @Override public void setStandardFontFamily(java.lang.String a0) { values.put("StandardFontFamily", a0); }
    @Override public void setSupportMultipleWindows(boolean a0) { values.put("SupportMultipleWindows", a0); }
    @Override public void setSupportZoom(boolean a0) { values.put("SupportZoom", a0); }
    @Override public void setTextZoom(int a0) { values.put("TextZoom", a0); }
    @Override public void setUseWideViewPort(boolean a0) { values.put("UseWideViewPort", a0); }
    @Override public void setUserAgentString(java.lang.String a0) { values.put("UserAgentString", a0); }
    @Override public boolean supportMultipleWindows() { Object v = values.get("SupportMultipleWindows"); return v != null ? (Boolean) v : false; }
    @Override public boolean supportZoom() { Object v = values.get("SupportZoom"); return v != null ? (Boolean) v : true; }
}
