#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <ass.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "../../src/vsfilter/csri.h"
#include "../../src/subtitles/RenderProfile.h"

// Shared implementations from render-bench.cpp, compiled with its entry point renamed.
uint64_t cpu_ticks();
uint64_t frame_hash(const std::vector<uint8_t>& pixels);
void blend_legacy(const ASS_Image* image, uint8_t* pixels);

namespace {
using Clock = std::chrono::steady_clock;
constexpr int width = 1920;
constexpr int height = 1080;
constexpr int pitch = width * 4;
constexpr int frameCount = 5400;
constexpr int fps = 60;
using ProfileReset = void (*)();
using ProfileRead = void (*)(uint64_t*, uint64_t*);

struct Playback {
    std::filesystem::path output;
    int events;
    size_t inputBytes;
    std::vector<int> captures;
    int firstFrame = 0;
    int endFrame = frameCount;
};

double elapsed_ms(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template<class T>
T lookup(HMODULE dll, const char* name)
{
    const auto address = GetProcAddress(dll, name);
    if (!address)
        throw std::runtime_error(std::string("Missing renderer export ") + name);
    return reinterpret_cast<T>(address);
}
#define API(name) const auto name = lookup<decltype(&::name)>(dll, #name)

void capture_frame(const std::filesystem::path& output, int index, const std::vector<uint8_t>& pixels)
{
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER image{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(image);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    image.biSize = sizeof(image);
    image.biWidth = width;
    image.biHeight = -height;
    image.biPlanes = 1;
    image.biBitCount = 32;
    image.biCompression = BI_RGB;

    std::ofstream bmp(output / ("frame-" + std::to_string(index) + ".bmp"), std::ios::binary);
    bmp.write(reinterpret_cast<const char*>(&file), sizeof(file));
    bmp.write(reinterpret_cast<const char*>(&image), sizeof(image));
    bmp.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    if (!bmp)
        throw std::runtime_error("Could not save frame capture");
}

template<class Render>
void play(const Playback& playback, Render render, double setupMs, int parsedEvents,
    ProfileReset reset = nullptr, ProfileRead read = nullptr)
{
    std::filesystem::create_directories(playback.output);
    std::ofstream frames(playback.output / "frames.csv");
    frames.exceptions(std::ios::badbit | std::ios::failbit);
    frames << "frame,presentation_ns,renderer_timestamp_ms,render_ms,cpu_ticks_100ns,rgb_hash\n";
    frames << std::fixed << std::setprecision(6);

    std::vector<uint8_t> pixels(pitch * height);
    const auto blank = frame_hash(pixels);
    uint64_t renderCpu = 0;
    int renderedVisibleFrames = 0;
    if (reset)
        reset();
    const auto passCpu = cpu_ticks();
    const auto passStart = Clock::now();
    for (int index = playback.firstFrame; index < playback.endFrame; ++index) {
        const int64_t presentationNs = int64_t(index) * 1000000000 / fps;
        const int renderTimestampMs = index * 1000 / fps;
        std::fill(pixels.begin(), pixels.end(), 0);
        const auto beforeCpu = cpu_ticks();
        const auto start = Clock::now();
        render(renderTimestampMs, pixels.data());
        const auto renderMs = elapsed_ms(start);
        const auto frameCpu = cpu_ticks() - beforeCpu;

        const auto hash = frame_hash(pixels);
        renderedVisibleFrames += hash != blank;
        renderCpu += frameCpu;
        frames << index << ',' << presentationNs << ',' << renderTimestampMs << ',' << renderMs
            << ',' << frameCpu << ',' << std::hex << std::setw(16) << std::setfill('0') << hash << std::dec << '\n';
        if (std::binary_search(playback.captures.begin(), playback.captures.end(), index))
            capture_frame(playback.output, index, pixels);
        if ((index + 1) % 600 == 0)
            std::cout << "Rendered through frame " << index + 1 << '/' << playback.endFrame << std::endl;
    }
    const auto passMs = elapsed_ms(passStart);
    const auto passCpuMs = (cpu_ticks() - passCpu) / 10000.0;
    frames.close();
    if (!renderedVisibleFrames)
        throw std::runtime_error("No visible output throughout the playback");

    PROCESS_MEMORY_COUNTERS memory{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    std::ofstream meta(playback.output / "metrics.json");
    meta << std::fixed << std::setprecision(6)
        << "{\"frames\":" << playback.endFrame - playback.firstFrame << ",\"setup_ms\":" << setupMs
        << ",\"input_bytes\":" << playback.inputBytes << ",\"parsed_events\":" << parsedEvents
        << ",\"visible_frames\":" << renderedVisibleFrames << ",\"render_cpu_ms\":" << renderCpu / 10000.0
        << ",\"pass_cpu_ms\":" << passCpuMs << ",\"pass_wall_ms\":" << passMs
        << ",\"peak_working_set_bytes\":" << memory.PeakWorkingSetSize;
    if (read) {
        constexpr auto stages = static_cast<unsigned>(RenderStage::Count);
        uint64_t ns[stages]{}, calls[stages]{};
        read(ns, calls);
        meta << ",\"profile\":[";
        for (unsigned i = 0; i < stages; ++i)
            meta << (i ? "," : "") << "{\"ns\":" << ns[i] << ",\"calls\":" << calls[i] << '}';
        meta << ']';
    }
    meta << "}\n";
    if (!meta)
        throw std::runtime_error("Could not save playback metrics");
}

void run_csri(HMODULE dll, const std::vector<char>& input, const Playback& playback)
{
    API(csri_renderer_default);
    API(csri_open_mem);
    API(csri_request_fmt);
    API(csri_render);
    API(csri_close);
    const auto start = Clock::now();
    const std::unique_ptr<csri_inst, decltype(csri_close)> instance(
        csri_open_mem(csri_renderer_default(), input.data(), input.size(), nullptr), csri_close);
    const csri_fmt format{CSRI_F_BGR_, width, height};
    if (!instance || csri_request_fmt(instance.get(), &format))
        throw std::runtime_error("CSRI rejected the input file or playback format");
    const auto reset = reinterpret_cast<ProfileReset>(GetProcAddress(dll, "csri_oborodzuki_profile_reset"));
    const auto read = reinterpret_cast<ProfileRead>(GetProcAddress(dll, "csri_oborodzuki_profile_read"));
    play(playback, [&](int timestamp, uint8_t* pixels) {
        csri_frame frame{};
        frame.pixfmt = format.pixfmt;
        frame.planes[0] = pixels;
        frame.strides[0] = pitch;
        // Both engines evaluate ASS at the same integer milliseconds. Nudge
        // CSRI's floating-point seconds past an exact truncation boundary.
        const auto seconds = std::nextafter(timestamp / 1000.0, (std::numeric_limits<double>::infinity)());
        csri_render(instance.get(), &frame, seconds);
    }, elapsed_ms(start), -1, reset, read);
    struct WorkerStats {
        unsigned long long batches, tasks, busyNs, wakeNs, waitNs, dispatchNs;
        unsigned long long preparationNs, tasksByWorker[8], busyByWorkerNs[8];
        unsigned long long idleNs, mutexNs;
        unsigned long long blendNs, blendBatches;
    } stats{};
    using ReadWorkers = void (*)(WorkerStats*, csri_inst*);
    const auto workers = reinterpret_cast<ReadWorkers>(GetProcAddress(dll, "csri_oborodzuki_worker_stats"));
    if (workers) {
        workers(&stats, instance.get());
        std::ofstream output(playback.output / "worker-stats.json");
        output << "{\"batches\":" << stats.batches << ",\"tasks\":" << stats.tasks
            << ",\"busy_ms\":" << stats.busyNs / 1e6 << ",\"wake_delay_ms\":" << stats.wakeNs / 1e6
            << ",\"completion_wait_ms\":" << stats.waitNs / 1e6
            << ",\"dispatch_ms\":" << stats.dispatchNs / 1e6
            << ",\"preparation_ms\":" << stats.preparationNs / 1e6
            << ",\"blend_ms\":" << stats.blendNs / 1e6 << ",\"blend_batches\":" << stats.blendBatches
            << ",\"worker_sleep_ms\":" << stats.idleNs / 1e6
            << ",\"mutex_acquisition_ms\":" << stats.mutexNs / 1e6 << ",\"workers\":[";
        for (unsigned i = 0; i < 8; ++i)
            output << (i ? "," : "") << "{\"tasks\":" << stats.tasksByWorker[i]
                << ",\"busy_ms\":" << stats.busyByWorkerNs[i] / 1e6 << "}";
        output << "]}\n";
    }
}

void run_mangetsu(HMODULE dll, std::vector<char>& input, const Playback& playback, bool rgba)
{
    API(ass_library_init);
    API(ass_library_done);
    API(ass_renderer_init);
    API(ass_renderer_done);
    API(ass_read_memory);
    API(ass_free_track);
    API(ass_set_frame_size);
    API(ass_set_storage_size);
    API(ass_set_fonts);
    API(ass_render_frame);
    API(ass_render_frame_rgba);
    API(ass_composite_images_bgra);
    API(ass_free_images_rgba);

    const auto start = Clock::now();
    const std::unique_ptr<ASS_Library, decltype(ass_library_done)> library(ass_library_init(), ass_library_done);
    if (!library)
        throw std::runtime_error("Mangetsu library initialization failed");
    const std::unique_ptr<ASS_Renderer, decltype(ass_renderer_done)> renderer(
        ass_renderer_init(library.get()), ass_renderer_done);
    const std::unique_ptr<ASS_Track, decltype(ass_free_track)> track(
        ass_read_memory(library.get(), input.data(), input.size(), nullptr), ass_free_track);
    if (!renderer || !track)
        throw std::runtime_error("Mangetsu rejected the input file");
    if (track->n_events != playback.events)
        throw std::runtime_error("Mangetsu loaded " + std::to_string(track->n_events) +
            " events, expected " + std::to_string(playback.events));
    ass_set_frame_size(renderer.get(), width, height);
    ass_set_storage_size(renderer.get(), width, height);
    ass_set_fonts(renderer.get(), nullptr, "Arial", ASS_FONTPROVIDER_DIRECTWRITE, nullptr, 1);

    play(playback, [&](int timestamp, uint8_t* pixels) {
        if (rgba) {
            const std::unique_ptr<ASS_ImageRGBA, decltype(ass_free_images_rgba)> images(
                ass_render_frame_rgba(renderer.get(), track.get(), timestamp, nullptr), ass_free_images_rgba);
            if (images && ass_composite_images_bgra(images.get(), pixels, width, height, pitch))
                throw std::runtime_error("Mangetsu RGBA compositing failed");
        } else {
            blend_legacy(ass_render_frame(renderer.get(), track.get(), timestamp, nullptr), pixels);
        }
    }, elapsed_ms(start), track->n_events);
}
#undef API
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 7) {
        std::cerr << "Usage: kfx-playback mode dll fixture output event-count input-bytes [capture-frame ...]\n";
        return 2;
    }
    try {
        Playback playback{argv[4], std::stoi(argv[5]), size_t(std::stoull(argv[6])), {}};
        for (int i = 7; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--window" && i + 2 < argc) {
                playback.firstFrame = std::stoi(argv[++i]);
                playback.endFrame = std::stoi(argv[++i]);
                if (playback.firstFrame < 0 || playback.endFrame > frameCount ||
                    playback.firstFrame >= playback.endFrame)
                    throw std::runtime_error("Invalid diagnostic frame window");
                continue;
            }
            const int frame = std::stoi(argv[i]);
            if (frame < 0 || frame >= frameCount)
                throw std::runtime_error("Invalid capture frame index");
            playback.captures.push_back(frame);
        }
        std::sort(playback.captures.begin(), playback.captures.end());

        std::ifstream file(std::filesystem::path(argv[3]), std::ios::binary);
        std::vector<char> input{std::istreambuf_iterator<char>(file), {}};
        if (input.empty() || input.size() != playback.inputBytes)
            throw std::runtime_error("Input byte count differs from the verified fixture");

        const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> dll(LoadLibraryW(argv[2]), FreeLibrary);
        if (!dll)
            throw std::runtime_error("LoadLibraryW failed with error " + std::to_string(GetLastError()));

        const std::wstring mode = argv[1];
        if (mode == L"csri")
            run_csri(dll.get(), input, playback);
        else if (mode == L"legacy" || mode == L"rgba")
            run_mangetsu(dll.get(), input, playback, mode == L"rgba");
        else
            throw std::runtime_error("Unknown renderer mode");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
