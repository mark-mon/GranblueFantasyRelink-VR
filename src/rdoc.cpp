// Optional RenderDoc in-application API (diagnostics): config "renderdoc_path = <dir or dll>".
#include <windows.h>
#include <string>
#include "renderdoc_app.h"
#include "globals.h"

static RENDERDOC_API_1_4_1* g_rdoc = nullptr;

void RDoc_Load() {
    if (g_cfg.renderdocPath.empty()) return;
    std::string p = g_cfg.renderdocPath;
    if (p.size() < 4 || _stricmp(p.c_str() + p.size() - 4, ".dll") != 0) p += "\\renderdoc.dll";
    HMODULE m = LoadLibraryA(p.c_str());
    if (!m) { LOGE("RenderDoc: cannot load %s (%lu)", p.c_str(), GetLastError()); return; }
    auto get = (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI");
    if (!get || !get(eRENDERDOC_API_Version_1_4_1, (void**)&g_rdoc)) { LOGE("RenderDoc: GetAPI failed"); g_rdoc = nullptr; return; }
    int a, b, c; g_rdoc->GetAPIVersion(&a, &b, &c);
    std::string tmpl = "D:/vrconversions/granbluevr/captures/gbvr";
    CreateDirectoryA("D:/vrconversions/granbluevr/captures", nullptr);
    g_rdoc->SetCaptureFilePathTemplate(tmpl.c_str());
    g_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 1);
    g_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_CaptureAllCmdLists, 1);
    g_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 0);
    g_rdoc->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    LOGI("RenderDoc: API %d.%d.%d loaded from %s; captures -> %s", a, b, c, p.c_str(), tmpl.c_str());
}

void RDoc_Trigger() {
    if (!g_rdoc) { LOGE("RenderDoc: not loaded"); return; }
    g_rdoc->TriggerCapture();
    LOGI("RenderDoc: capture triggered");
}

void RDoc_Poll() {
    static uint32_t seen = 0;
    if (!g_rdoc) return;
    uint32_t n = g_rdoc->GetNumCaptures();
    while (seen < n) {
        char path[512]; uint32_t len = sizeof(path); uint64_t ts;
        if (g_rdoc->GetCapture(seen, path, &len, &ts)) LOGI("RenderDoc: capture %u written: %s", seen, path);
        ++seen;
    }
}
