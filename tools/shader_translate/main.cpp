#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "shader_recompiler.h"

static std::vector<uint8_t> ReadFile(const char *path) {
  std::ifstream file(path, std::ios::binary);
  file.exceptions(std::ios::badbit);
  if (!file)
    throw std::runtime_error(std::string("Cannot read ") + path);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: daytona_shader_translate INPUT OUTPUT HEADER\n";
    return 2;
  }
  try {
    const auto shader = ReadFile(argv[1]);
    const auto header = ReadFile(argv[3]);
    if (shader.empty() || header.empty())
      throw std::runtime_error("Empty shader container or common header");
    ShaderRecompiler recompiler;
    recompiler.recompile(
        shader.data(),
        {reinterpret_cast<const char *>(header.data()), header.size()});
    std::ofstream output(argv[2], std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(recompiler.out.data(), recompiler.out.size());
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
