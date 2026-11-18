// Loads the packaged renderer through its public CSRI ABI.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "../../src/vsfilter/csri.h"

template <class T>
T load_export(HMODULE dll, const char* name)
{
    auto address = GetProcAddress(dll, name);
    if (!address)
        throw std::runtime_error(name);
#pragma warning(suppress: 4191)
    return reinterpret_cast<T>(address);
}

#define CSRI_EXPORT(name) const auto name = load_export<decltype(&::name)>(dll, #name)

constexpr char script[] = R"ass([Script Info]
ScriptType: v4.00+
PlayResX: 320
PlayResY: 180

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,28,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,1,2,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,Oborodzuki
)ass";

void verify_renderer(HMODULE dll)
{
    CSRI_EXPORT(csri_renderer_default);
    CSRI_EXPORT(csri_renderer_byname);
    CSRI_EXPORT(csri_renderer_info);
    CSRI_EXPORT(csri_renderer_next);
    CSRI_EXPORT(csri_query_ext);
    CSRI_EXPORT(csri_open_file);
    CSRI_EXPORT(csri_open_mem);
    CSRI_EXPORT(csri_request_fmt);
    CSRI_EXPORT(csri_render);
    CSRI_EXPORT(csri_close);

    const char* exports[] = {
        "DllCanUnloadNow", "DllGetClassObject", "DllRegisterServer",
        "DllUnregisterServer", "DirectVobSub", "VirtualdubFilterModuleInit2",
        "VirtualdubFilterModuleDeinit"
    };
    for (const char* name : exports)
        load_export<FARPROC>(dll, name);
#ifdef _WIN64
    load_export<FARPROC>(dll, "VapourSynthPluginInit");
    load_export<FARPROC>(dll, "AvisynthPluginInit2");
#else
    load_export<FARPROC>(dll, "_VapourSynthPluginInit@12");
    load_export<FARPROC>(dll, "_AvisynthPluginInit2@4");
#endif

    auto renderer = csri_renderer_default();
    if (!renderer)
        throw std::runtime_error("No default renderer");
    const auto info = csri_renderer_info(renderer);
    if (!info || std::strcmp(info->name, "vsfiltermod_oborodzuki") ||
        std::strcmp(info->longname, "vsfiltermod oborodzuki") ||
        csri_renderer_byname(info->name, info->specific) != renderer ||
        csri_renderer_byname("vsfiltermod_textsub", info->specific) != renderer ||
        csri_renderer_next(renderer))
        throw std::runtime_error("Unexpected CSRI renderer identity");

    auto instance = csri_open_mem(renderer, script, sizeof(script) - 1, nullptr);
    if (!instance)
        throw std::runtime_error("Could not open the ASS fixture");

    constexpr csri_fmt format{CSRI_F_BGR_, 320, 180};
    const std::vector<unsigned char> blank(format.width * format.height * 4, 0);
    auto pixels = blank;
    csri_frame frame{};
    frame.pixfmt = format.pixfmt;
    frame.planes[0] = pixels.data();
    frame.strides[0] = format.width * 4;

    int request = csri_request_fmt(instance, &format);
    bool visible = false;
    bool correct_after_seek = false;
    if (request == 0) {
        csri_render(instance, &frame, 1.5);
        for (size_t i = 0; i < pixels.size(); i += 4)
            visible |= pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0;

        const auto expected = pixels;
        std::fill(pixels.begin(), pixels.end(), 0);
        csri_render(instance, &frame, 0.5);
        const bool idle = pixels == blank;
        std::fill(pixels.begin(), pixels.end(), 0);
        csri_render(instance, &frame, 1.5);
        correct_after_seek = idle && pixels == expected;
    }
    csri_close(instance);
    if (request != 0 || !visible || !correct_after_seek)
        throw std::runtime_error("CSRI rendering did not match the fixture expectations");

    std::printf("%s (%s) loaded and rendered the ASS fixture.\n", info->longname, info->specific);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
        return 2;
    auto dll = LoadLibraryW(argv[1]);
    if (!dll) {
        std::fprintf(stderr, "LoadLibraryW failed with error %lu\n", GetLastError());
        return 1;
    }
    int result = 0;
    try {
        verify_renderer(dll);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CSRI smoke test failed: %s\n", error.what());
        result = 1;
    }
    FreeLibrary(dll);
    return result;
}
