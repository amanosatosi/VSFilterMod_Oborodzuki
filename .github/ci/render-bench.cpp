#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
#include <ass.h>
#include "../../src/vsfilter/csri.h"
#include "../../src/subtitles/RenderProfile.h"

using Clock = std::chrono::steady_clock;
constexpr int width = 1920;
constexpr int height = 1080;
constexpr int pitch = width * 4;

template <class Function>
Function symbol(HMODULE dll, const char* name)
{
    auto proc = GetProcAddress(dll, name);
    if (!proc)
        throw std::runtime_error(name);
    return reinterpret_cast<Function>(proc);
}

#define API(name) const auto name = symbol<decltype(&::name)>(dll, #name)

double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

uint64_t cpu_ticks()
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error("GetProcessTimes failed");
    auto ticks = [](FILETIME t) {
        return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return ticks(kernel) + ticks(user);
}

void blend_legacy(const ASS_Image* image, uint8_t* pixels)
{
    for (; image; image = image->next) {
        const int x0 = std::max(0, image->dst_x);
        const int y0 = std::max(0, image->dst_y);
        const int x1 = std::min(width, image->dst_x + image->w);
        const int y1 = std::min(height, image->dst_y + image->h);
        const unsigned opacity = 255 - (image->color & 255);
        const unsigned color[] = { (image->color >> 8) & 255,
            (image->color >> 16) & 255, image->color >> 24 };
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const unsigned a = image->bitmap[(y - image->dst_y) * image->stride +
                    x - image->dst_x] * opacity / 255;
                auto dst = pixels + y * pitch + x * 4;
                for (int c = 0; c < 3; ++c)
                    dst[c] = uint8_t((dst[c] * (255 - a) + color[c] * a + 127) / 255);
            }
        }
    }
}

uint64_t frame_hash(const std::vector<uint8_t>& pixels)
{
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < pixels.size(); ++i) {
        if (i % 4 != 3)
            hash = (hash ^ pixels[i]) * 1099511628211ULL;
    }
    return hash;
}

using ProfileReset = void (*)();
using ProfileRead = void (*)(uint64_t*, uint64_t*);

struct Workload {
    std::wstring name;
    int first_ms = 0;
    int stop_ms = 0;
};

template <class Render>
void measure(Render render, const Workload& workload, double setup_ms,
    ProfileReset reset_profile = nullptr, ProfileRead read_profile = nullptr)
{
    std::vector<uint8_t> pixels(pitch * height);
    std::vector<double> timings;
    std::vector<uint64_t> hashes;
    const bool real = workload.name == L"real";
    const bool regression = workload.name == L"regression";
    const bool iteration = std::getenv("OBORODZUKI_BENCH_ITERATION") != nullptr;
    const int count = regression ? 20 : real ? (iteration ? 60 : 240) : (iteration ? 120 : 600);
    auto time_at = [&workload, real, count, iteration](int i) {
        if (real)
            return workload.first_ms + int(int64_t(workload.stop_ms - workload.first_ms - 1) * i / (count - 1));
        if (workload.name == L"static") return 1500;
        if (workload.name == L"idle") return 0;
        if (workload.name == L"seek") return 1000 + (i * 1879 % 4000);
        if (iteration && workload.name != L"regression") return 1000 + i * 3999 / (count - 1);
        return 1000 + i * 20 % 4000;
    };
    auto one_frame = [&](int t) {
        std::fill(pixels.begin(), pixels.end(), 0);
        const auto cpu = cpu_ticks();
        const auto start = Clock::now();
        render(t, pixels.data());
        const double ms = elapsed(start);
        return std::pair<double, uint64_t>{ms, cpu_ticks() - cpu};
    };
    const double first = one_frame(time_at(real ? count / 2 : 0)).first;
    const int warmup = regression ? 0 : real ? (iteration ? 6 : 12) : (iteration ? 10 : 30);
    for (int i = 0; i < warmup; ++i)
        one_frame(time_at(real ? i * (count - 1) / (warmup - 1) : i));
    if (reset_profile)
        reset_profile();
    uint64_t cpu = 0;
    for (int i = 0; i < count; ++i) {
        const auto [ms, ticks] = one_frame(time_at(i));
        timings.push_back(ms);
        cpu += ticks;
    }
    constexpr auto stages = static_cast<unsigned>(RenderStage::Count);
    uint64_t ns[stages]{}, calls[stages]{};
    if (read_profile)
        read_profile(ns, calls);

    // Hash a separate set of frames outside the timed loop.
    std::vector<uint8_t> background(pixels.size());
    for (size_t i = 0; i < background.size(); i += 4) {
        background[i] = uint8_t(i >> 2);
        background[i + 1] = uint8_t(i >> 7);
        background[i + 2] = uint8_t(i >> 12);
        background[i + 3] = 255;
    }
    const uint64_t background_hash = frame_hash(background);
    const std::vector<int> checks = real
        ? std::vector<int>{time_at(0), time_at(count / 3), time_at(count / 2),
            time_at(count * 2 / 3), time_at(count / 4), time_at(count - 1), time_at(count / 2)}
        : std::vector<int>{0, 1000, 1500, 3200, 2600, 5200, 1500};
    for (int t : checks) {
        std::copy(background.begin(), background.end(), pixels.begin());
        render(t, pixels.data());
        hashes.push_back(frame_hash(pixels));
    }
    if (std::all_of(hashes.begin(), hashes.end(), [&](uint64_t hash) { return hash == background_hash; }))
        throw std::runtime_error("Fixture produced no visible pixels at any verification timestamp");
    if (regression && hashes[2] != hashes.back())
        throw std::runtime_error("Rendered pixels changed after seeking back to 1500 ms");

    std::sort(timings.begin(), timings.end());
    const double total = std::accumulate(timings.begin(), timings.end(), 0.0);
    const double cpu_ms = cpu / 10000.0;
    PROCESS_MEMORY_COUNTERS memory{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    std::printf("{\"frames\":%d,\"warmup_frames\":%d,\"setup_ms\":%.6f,\"first_frame_ms\":%.6f,\"avg_ms\":%.6f,"
        "\"median_ms\":%.6f,\"p95_ms\":%.6f,\"fps\":%.3f,\"cpu_ms\":%.3f,"
        "\"cpu_percent_of_one_core\":%.3f,\"peak_working_set_bytes\":%llu,\"hashes\":[",
        count, warmup, setup_ms, first, total / count, (timings[count / 2 - 1] + timings[count / 2]) / 2,
        timings[(count * 95 + 99) / 100 - 1], 1000 * count / total, cpu_ms, 100 * cpu_ms / total,
        static_cast<unsigned long long>(memory.PeakWorkingSetSize));
    for (size_t i = 0; i < hashes.size(); ++i)
        std::printf("%s\"%016llx\"", i ? "," : "", static_cast<unsigned long long>(hashes[i]));
    std::printf("]");
    if (read_profile) {
        std::printf(",\"profile\":[");
        for (unsigned i = 0; i < stages; ++i)
            std::printf("%s{\"ns\":%llu,\"calls\":%llu}", i ? "," : "",
                static_cast<unsigned long long>(ns[i]), static_cast<unsigned long long>(calls[i]));
        std::printf("]");
    }
    std::puts("}");
}

void run_csri(HMODULE dll, const std::vector<char>& script, const Workload& workload)
{
    API(csri_renderer_default);
    API(csri_open_mem);
    API(csri_request_fmt);
    API(csri_render);
    API(csri_close);
    const auto start = Clock::now();
    auto instance = csri_open_mem(csri_renderer_default(), script.data(), script.size(), nullptr);
    constexpr csri_fmt format{CSRI_F_BGR_, width, height};
    if (!instance || csri_request_fmt(instance, &format))
        throw std::runtime_error("CSRI initialization failed");
    auto reset = reinterpret_cast<ProfileReset>(GetProcAddress(dll, "csri_oborodzuki_profile_reset"));
    auto read = reinterpret_cast<ProfileRead>(GetProcAddress(dll, "csri_oborodzuki_profile_read"));
    measure([&](int t, uint8_t* pixels) {
        csri_frame frame{};
        frame.pixfmt = format.pixfmt;
        frame.planes[0] = pixels;
        frame.strides[0] = pitch;
        csri_render(instance, &frame, t / 1000.0);
    }, workload, elapsed(start), reset, read);
    csri_close(instance);
}

void run_mangetsu(HMODULE dll, std::vector<char>& script, const Workload& workload, bool rgba)
{
    API(ass_library_init);
    API(ass_library_done);
    API(ass_renderer_init);
    API(ass_renderer_done);
    API(ass_set_frame_size);
    API(ass_set_storage_size);
    API(ass_set_fonts);
    API(ass_read_memory);
    API(ass_free_track);
    API(ass_render_frame);
    API(ass_render_frame_rgba);
    API(ass_composite_images_bgra);
    API(ass_free_images_rgba);

    const auto start = Clock::now();
    auto library = ass_library_init();
    if (!library)
        throw std::runtime_error("Mangetsu library initialization failed");
    auto renderer = ass_renderer_init(library);
    auto track = ass_read_memory(library, script.data(), script.size(), nullptr);
    if (!renderer || !track)
        throw std::runtime_error("Mangetsu renderer initialization failed");
    ass_set_frame_size(renderer, width, height);
    ass_set_storage_size(renderer, width, height);
    ass_set_fonts(renderer, nullptr, "Arial", ASS_FONTPROVIDER_DIRECTWRITE, nullptr, 1);

    measure([&](int t, uint8_t* pixels) {
        if (rgba) {
            auto images = ass_render_frame_rgba(renderer, track, t, nullptr);
            if (images) {
                const int result = ass_composite_images_bgra(images, pixels, width, height, pitch);
                ass_free_images_rgba(images);
                if (result)
                    throw std::runtime_error("Mangetsu RGBA compositing failed");
            }
        } else {
            blend_legacy(ass_render_frame(renderer, track, t, nullptr), pixels);
        }
    }, workload, elapsed(start));

    ass_free_track(track);
    ass_renderer_done(renderer);
    ass_library_done(library);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5 && argc != 7)
        return 2;
    try {
        std::ifstream file(std::filesystem::path(argv[3]), std::ios::binary);
        std::vector<char> script{std::istreambuf_iterator<char>(file), {}};
        if (script.empty())
            throw std::runtime_error("Missing ASS fixture");
        const auto dll = LoadLibraryW(argv[2]);
        if (!dll)
            throw std::runtime_error("Could not load renderer DLL");
        const std::wstring mode = argv[1];
        Workload workload{argv[4]};
        if (workload.name == L"real") {
            if (argc != 7)
                throw std::runtime_error("The real workload requires a timestamp range");
            workload.first_ms = std::stoi(argv[5]);
            workload.stop_ms = std::stoi(argv[6]);
            if (workload.first_ms < 0 || workload.stop_ms <= workload.first_ms)
                throw std::runtime_error("Invalid real workload timestamp range");
        }
        if (mode == L"csri")
            run_csri(dll, script, workload);
        else if (mode == L"legacy" || mode == L"rgba")
            run_mangetsu(dll, script, workload, mode == L"rgba");
        else
            throw std::runtime_error("Unknown renderer mode");
        FreeLibrary(dll);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
