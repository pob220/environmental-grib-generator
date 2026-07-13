#include "environmental_grib/random_access.h"

#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <utility>

#include "environmental_grib/error.h"

namespace environmental_grib {
namespace {

std::uint64_t CheckedEnd(std::uint64_t offset, std::uint64_t length,
                         std::uint64_t limit, const std::string& description) {
  if (length > std::numeric_limits<std::uint64_t>::max() - offset ||
      offset + length > limit) {
    throw ValidationError(description + " is outside its bounded source");
  }
  return offset + length;
}

class FileSource final : public RandomAccessSource {
public:
  explicit FileSource(std::filesystem::path path)
      : path_(std::move(path)), input_(path_, std::ios::binary) {
    if (!input_) {
      throw ValidationError("could not read package " + path_.string());
    }
    std::error_code error;
    size_ = std::filesystem::file_size(path_, error);
    if (error) {
      throw ValidationError("could not determine package size " +
                            path_.string());
    }
  }

  std::uint64_t size() const noexcept override { return size_; }
  std::uint64_t bytes_read() const noexcept override { return bytes_read_; }
  std::string label() const override { return path_.string(); }

  std::vector<unsigned char> Read(std::uint64_t offset, std::size_t length,
                                  const std::string& description) override {
    CheckedEnd(offset, length, size_, description);
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max()) ||
        length > static_cast<std::size_t>(
                     std::numeric_limits<std::streamsize>::max())) {
      throw ValidationError(description + " is not representable");
    }
    std::vector<unsigned char> result(length);
    std::lock_guard lock(mutex_);
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input_ ||
        (length != 0 && !input_.read(reinterpret_cast<char*>(result.data()),
                                     static_cast<std::streamsize>(length)))) {
      throw ValidationError("could not read " + description + " from " +
                            path_.string());
    }
    bytes_read_.fetch_add(length, std::memory_order_relaxed);
    return result;
  }

private:
  std::filesystem::path path_;
  std::ifstream input_;
  std::uint64_t size_{};
  std::atomic<std::uint64_t> bytes_read_{};
  std::mutex mutex_;
};

class BoundedSource final : public RandomAccessSource {
public:
  BoundedSource(std::shared_ptr<RandomAccessSource> parent,
                std::uint64_t offset, std::uint64_t length, std::string label)
      : parent_(std::move(parent)),
        offset_(offset),
        length_(length),
        label_(std::move(label)) {
    if (!parent_) throw ValidationError("bounded source has no parent");
    CheckedEnd(offset_, length_, parent_->size(), "bounded source");
  }

  std::uint64_t size() const noexcept override { return length_; }
  std::uint64_t bytes_read() const noexcept override {
    return parent_->bytes_read();
  }
  std::string label() const override { return label_; }

  std::vector<unsigned char> Read(std::uint64_t offset, std::size_t length,
                                  const std::string& description) override {
    CheckedEnd(offset, length, length_, description);
    return parent_->Read(offset_ + offset, length,
                         description + " in " + label_);
  }

private:
  std::shared_ptr<RandomAccessSource> parent_;
  std::uint64_t offset_{};
  std::uint64_t length_{};
  std::string label_;
};

}  // namespace

std::shared_ptr<RandomAccessSource> OpenFileSource(
    const std::filesystem::path& path) {
  return std::make_shared<FileSource>(path);
}

std::shared_ptr<RandomAccessSource> MakeBoundedSource(
    std::shared_ptr<RandomAccessSource> parent, std::uint64_t offset,
    std::uint64_t length, std::string label) {
  return std::make_shared<BoundedSource>(std::move(parent), offset, length,
                                         std::move(label));
}

}  // namespace environmental_grib
