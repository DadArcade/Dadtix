#include <esp_log.h>
#include "ZipFile.h"
#include "miniz_custom.h"

#define TAG "ZIP"

struct ZipFile::Impl
{
  mz_zip_archive zip_archive;
  bool is_open = false;

  Impl()
  {
    memset(&zip_archive, 0, sizeof(zip_archive));
  }

  ~Impl()
  {
    close();
  }

  bool open(const char *filename)
  {
    if (is_open) return true;

    memset(&zip_archive, 0, sizeof(zip_archive));
    is_open = mz_zip_reader_init_file(&zip_archive, filename, 0);
    if (!is_open)
    {
      ESP_LOGE(TAG, "mz_zip_reader_init_file() failed for %s!\n", filename);
      ESP_LOGE(TAG, "Error %s\n", mz_zip_get_error_string(zip_archive.m_last_error));
      return false;
    }
    return true;
  }

  void close()
  {
    if (is_open)
    {
      mz_zip_reader_end(&zip_archive);
      is_open = false;
    }
  }
};

ZipFile::ZipFile(const char *filename)
  : m_filename(filename), m_impl(new Impl())
{
}

ZipFile::~ZipFile() = default;

ZipFile::ZipFile(ZipFile &&) noexcept = default;
ZipFile &ZipFile::operator=(ZipFile &&) noexcept = default;

bool ZipFile::open()
{
  return m_impl->open(m_filename.c_str());
}

void ZipFile::close()
{
  if (m_impl)
  {
    m_impl->close();
  }
}

bool ZipFile::is_open() const
{
  return m_impl ? m_impl->is_open : false;
}

bool ZipFile::ensure_open()
{
  if (!is_open())
  {
    return open();
  }
  return true;
}

uint8_t *ZipFile::read_file_to_memory(const char *filename, size_t *size)
{
  if (!ensure_open())
  {
    return nullptr;
  }

  // find the file
  mz_uint32 file_index = 0;
  if (!mz_zip_reader_locate_file_v2(&m_impl->zip_archive, filename, nullptr, 0, &file_index))
  {
    ESP_LOGE(TAG, "Could not find file %s", filename);
    return nullptr;
  }

  // get the file size - we do this all manually so we can add a null terminator to any strings
  mz_zip_archive_file_stat file_stat;
  if (!mz_zip_reader_file_stat(&m_impl->zip_archive, file_index, &file_stat))
  {
    ESP_LOGE(TAG, "mz_zip_reader_file_stat() failed!\n");
    ESP_LOGE(TAG, "Error %s\n", mz_zip_get_error_string(m_impl->zip_archive.m_last_error));
    return nullptr;
  }

  // allocate memory for the file
  size_t file_size = file_stat.m_uncomp_size;
  uint8_t *file_data = (uint8_t *)calloc(file_size + 1, 1);
  if (!file_data)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for %s\n", file_stat.m_filename);
    return nullptr;
  }

  // read the file
  bool status = mz_zip_reader_extract_to_mem(&m_impl->zip_archive, file_index, file_data, file_size, 0);
  if (!status)
  {
    ESP_LOGE(TAG, "mz_zip_reader_extract_to_mem() failed!\n");
    ESP_LOGE(TAG, "Error %s\n", mz_zip_get_error_string(m_impl->zip_archive.m_last_error));
    free(file_data);
    return nullptr;
  }

  // return the size if required
  if (size)
  {
    *size = file_size;
  }
  return file_data;
}

bool ZipFile::read_file_to_file(const char *filename, const char *dest)
{
  if (!ensure_open())
  {
    return false;
  }

  mz_uint32 file_index = 0;
  if (!mz_zip_reader_locate_file_v2(&m_impl->zip_archive, filename, nullptr, 0, &file_index))
  {
    ESP_LOGE(TAG, "Could not find file %s", filename);
    return false;
  }

  return mz_zip_reader_extract_to_file(&m_impl->zip_archive, file_index, dest, 0);
}
