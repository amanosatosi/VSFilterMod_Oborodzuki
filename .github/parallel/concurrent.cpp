#define NOMINMAX
#define wmain original_smoke_main
#include "../ci/csri-smoke.cpp"
#undef wmain
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
        return 2;
    const auto dll = LoadLibraryW(argv[1]);
    if (!dll)
        return 1;
    try
    {
        verify_renderer(dll);
        const auto renderer = load_export<decltype(&csri_renderer_default)>(dll, "csri_renderer_default");
        const auto open = load_export<decltype(&csri_open_mem)>(dll, "csri_open_mem");
        const auto format = load_export<decltype(&csri_request_fmt)>(dll, "csri_request_fmt");
        const auto render = load_export<decltype(&csri_render)>(dll, "csri_render");
        const auto close = load_export<decltype(&csri_close)>(dll, "csri_close");
        std::ifstream file(std::filesystem::path(argv[2]), std::ios::binary);
        const std::vector<char> data{std::istreambuf_iterator<char>(file), {}};
        if (data.empty())
            throw std::runtime_error("Missing concurrency fixture");
        const double times[] = {1.5, 3.2, 1.2, 0.0, 2.8, 1.5};
        auto draw = [&](csri_inst* instance, int index) {
            const unsigned width = index % 2 ? 640 : 320, height = width * 9 / 16;
            const csri_fmt fmt{CSRI_F_BGR_, width, height};
            if (format(instance, &fmt))
                throw std::runtime_error("Resolution change failed");
            std::vector<unsigned char> pixels(width * height * 4);
            csri_frame frame{};
            frame.pixfmt = fmt.pixfmt;
            frame.planes[0] = pixels.data();
            frame.strides[0] = width * 4;
            render(instance, &frame, times[index]);
            return pixels;
        };
        SetEnvironmentVariableW(L"OBORODZUKI_WORKERS", L"1");
        auto reference = open(renderer(), data.data(), data.size(), nullptr);
        if (!reference)
            throw std::runtime_error("Failed to open concurrency fixture");
        std::vector<std::vector<unsigned char>> expected;
        for (int index = 0; index < 6; ++index)
            expected.push_back(draw(reference, index));
        close(reference);
        const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);

        SetEnvironmentVariableW(L"OBORODZUKI_WORKERS", L"4");
        std::atomic<bool> failed{false};
        std::vector<std::thread> threads;
        for (int worker = 0; worker < 4; ++worker)
            threads.emplace_back([&] {
                try {
                    for (int repeat = 0; repeat < 10; ++repeat) {
                        auto instance = open(renderer(), data.data(), data.size(), nullptr);
                        if (!instance)
                            throw std::runtime_error("Failed to initialize concurrent instance");
                        bool matches = true;
                        for (int index = 0; index < 6; ++index)
                            matches = matches && draw(instance, index) == expected[index];
                        close(instance);
                        if (!matches)
                            throw std::runtime_error("Concurrent render pixels differ from one worker");
                    }
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "%s\n", error.what());
                    failed = true;
                }
            });
        for (auto& thread : threads)
            thread.join();
        const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        std::printf("Concurrent instances, seeks, resizing and teardown: GDI %lu -> %lu\n", gdiBefore, gdiAfter);
        if (failed || gdiAfter > gdiBefore + 4)
            throw std::runtime_error("Concurrent instance validation failed");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        FreeLibrary(dll);
        return 1;
    }
    FreeLibrary(dll);
    return 0;
}
