#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace environmental_grib {

class RandomAccessSource {
public:
  virtual ~RandomAccessSource() = default;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t bytes_read() const noexcept = 0;
  [[nodiscard]] virtual std::string label() const = 0;
  virtual std::vector<unsigned char> Read(std::uint64_t offset,
                                          std::size_t length,
                                          const std::string& description) = 0;
};

std::shared_ptr<RandomAccessSource> OpenFileSource(
    const std::filesystem::path& path);
std::shared_ptr<RandomAccessSource> MakeBoundedSource(
    std::shared_ptr<RandomAccessSource> parent, std::uint64_t offset,
    std::uint64_t length, std::string label);

}  // namespace environmental_grib
