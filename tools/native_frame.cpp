#include "native_renderer.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <string_view>
int main(int argc, char **argv) {
  if (argc != 4 && argc != 5) {
    std::cerr << "usage: daytona_native_frame FRAME SHADERS OUTPUT.rgba "
                 "[--repeat=N]\n";
    return 2;
  }
  try {
    daytona::native::Renderer renderer;
    auto frame = daytona::native::LoadFrame(argv[1]);
    uint32_t repeat = 1;
    if (argc == 5) {
      std::string_view option(argv[4]);
      if (!option.starts_with("--repeat="))
        throw std::runtime_error("Expected --repeat=N");
      option.remove_prefix(9);
      auto [end, error] =
          std::from_chars(option.data(), option.data() + option.size(), repeat);
      if (error != std::errc{} || end != option.data() + option.size() ||
          repeat == 0)
        throw std::runtime_error("Invalid repeat count");
    }
    std::vector<uint8_t> pixels;
    double record_ms = 0, queue_ms = 0;
    for (uint32_t i = 0; i < repeat; ++i) {
      daytona::native::RenderTimings timing;
      pixels = renderer.Render(frame, argv[2], {}, &timing);
      if (i) {
        record_ms += timing.record_ms;
        queue_ms += timing.queue_ms;
      }
    }
    if (repeat > 1)
      std::cout << "Steady frame ms: record " << record_ms / (repeat - 1)
                << ", queue " << queue_ms / (repeat - 1) << '\n';
    std::ofstream output(argv[3], std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(reinterpret_cast<const char *>(pixels.data()), pixels.size());
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
