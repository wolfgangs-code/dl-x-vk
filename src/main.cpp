#include <csignal>
#include <iostream>
#include <chrono>
#include <thread>
#include <iomanip>
#include "common.hpp"
#include "service.hpp"
#include "color_converter.hpp"
#include "vulkan_converter.hpp"

static std::unique_ptr<dl_turbo::DisplayLinkService> g_service;

void SignalHandler(int signum) {
    LOG_INFO("Caught signal %d, shutting down cleanly...", signum);
    if (g_service) {
        g_service->Stop();
    }
}

void RunBenchmark() {
    LOG_INFO("==================================================");
    LOG_INFO(" Running Color Conversion & Throughput Benchmark");
    LOG_INFO(" Active Backend Engine: %s", dl_turbo::ColorConverter::GetAccelerationEngine());
    if (dl_turbo::ColorConverter::IsVulkanAccelerated()) {
        LOG_INFO(" Vulkan Compute Device: %s", dl_turbo::VulkanConverter::Instance().GetDeviceName().c_str());
    }
    LOG_INFO("==================================================");

    constexpr int WIDTH = 3840;
    constexpr int HEIGHT = 2160;
    constexpr int FRAMES = 100;

    std::vector<uint8_t> rgb_buffer(WIDTH * HEIGHT * 4, 0x7F);
    std::vector<uint8_t> y_plane(WIDTH * HEIGHT);
    std::vector<uint8_t> u_plane(WIDTH * HEIGHT / 4);
    std::vector<uint8_t> v_plane(WIDTH * HEIGHT / 4);

    // Warmup frame
    dl_turbo::ColorConverter::Rgb32ToYuv420(
        rgb_buffer.data(), WIDTH * 4, WIDTH, HEIGHT,
        y_plane.data(), u_plane.data(), v_plane.data(),
        WIDTH, WIDTH / 2
    );

    // 1. End-to-end benchmark
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < FRAMES; ++i) {
        dl_turbo::ColorConverter::Rgb32ToYuv420(
            rgb_buffer.data(), WIDTH * 4, WIDTH, HEIGHT,
            y_plane.data(), u_plane.data(), v_plane.data(),
            WIDTH, WIDTH / 2
        );
    }
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

    LOG_INFO("[End-to-End Frame Pipeline]");
    LOG_INFO("  Processed %d 4K (3840x2160) frames in %.2f ms", FRAMES, elapsed_ms);
    LOG_INFO("  Throughput: %.2f FPS (%.2f ms/frame)", (FRAMES * 1000.0) / elapsed_ms, elapsed_ms / FRAMES);

    // 2. Direct GPU Compute Kernel Dispatch (Zero-Copy)
    if (dl_turbo::ColorConverter::IsVulkanAccelerated()) {
        auto& vk = dl_turbo::VulkanConverter::Instance();
        auto start_kernel = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < FRAMES; ++i) {
            vk.DispatchCompute(WIDTH, HEIGHT, WIDTH * 4, WIDTH, WIDTH / 2);
        }
        auto end_kernel = std::chrono::high_resolution_clock::now();
        double kernel_ms = std::chrono::duration<double, std::milli>(end_kernel - start_kernel).count();

        LOG_INFO("[Vulkan GPU Compute Kernel (Zero-Copy Mapped)]");
        LOG_INFO("  Processed %d 4K frames in %.2f ms", FRAMES, kernel_ms);
        LOG_INFO("  Peak GPU Compute: %.2f FPS (%.2f ms/frame)", (FRAMES * 1000.0) / kernel_ms, kernel_ms / FRAMES);
    }

    // Verify sample output values
    LOG_INFO("Sample Verification: Y=%u, U=%u, V=%u",
             static_cast<unsigned>(y_plane[0]),
             static_cast<unsigned>(u_plane[0]),
             static_cast<unsigned>(v_plane[0]));
}

int main(int argc, char* argv[]) {
    bool run_benchmark = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-debug") {
            dl_turbo::g_log_level = dl_turbo::LogLevel::DEBUG;
        } else if (arg == "-logging") {
            dl_turbo::g_log_level = dl_turbo::LogLevel::INFO;
        } else if (arg == "-version" || arg == "--version") {
            std::cout << "dl-x-vk v1.1.0 (DisplayLink Turbo Vulkan/SPIR-V Edition)" << std::endl;
            std::cout << "Built with EVDI 1.15.1, libusb-1.0, and Vulkan SPIR-V compute acceleration" << std::endl;
            return 0;
        } else if (arg == "--benchmark") {
            run_benchmark = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: dl-x-vk [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -debug       Enable debug logging" << std::endl;
            std::cout << "  -logging     Log to console (INFO level)" << std::endl;
            std::cout << "  -version     Display version info" << std::endl;
            std::cout << "  --benchmark  Run 4K color conversion benchmark" << std::endl;
            return 0;
        }
    }

    if (run_benchmark) {
        RunBenchmark();
        return 0;
    }

    // Set signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    LOG_INFO("==================================================");
    LOG_INFO(" dl-x-vk DisplayLink Turbo Service starting");
    LOG_INFO(" High-performance Vulkan-accelerated DisplayLink daemon");
    LOG_INFO(" Acceleration Engine: %s", dl_turbo::ColorConverter::GetAccelerationEngine());
    if (dl_turbo::ColorConverter::IsVulkanAccelerated()) {
        LOG_INFO(" Compute GPU: %s", dl_turbo::VulkanConverter::Instance().GetDeviceName().c_str());
    }
    LOG_INFO("==================================================");

    g_service = std::make_unique<dl_turbo::DisplayLinkService>();

    if (!g_service->Start()) {
        LOG_ERROR("Failed to start DisplayLink service");
        return 1;
    }

    // Wait until stopped
    g_service->Join();

    return 0;
}
