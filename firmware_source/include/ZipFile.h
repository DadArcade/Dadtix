#pragma once

#include <string>
#include <memory>

class ZipFile
{
private:
  std::string m_filename;
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  bool ensure_open();

public:
  ZipFile(const char *filename);
  ~ZipFile();

  // Prevent copying because of the underlying zip archive and file handle
  ZipFile(const ZipFile &) = delete;
  ZipFile &operator=(const ZipFile &) = delete;

  ZipFile(ZipFile &&) noexcept;
  ZipFile &operator=(ZipFile &&) noexcept;

  bool open();
  void close();
  bool is_open() const;

  // read a file from the zip file allocating the required memory for the data
  uint8_t *read_file_to_memory(const char *filename, size_t *size = nullptr);
  bool read_file_to_file(const char *filename, const char *dest);
};