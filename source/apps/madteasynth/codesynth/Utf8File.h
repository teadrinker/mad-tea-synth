#pragma once

// Paths are UTF-8. A narrow std::string handed to fstream/filesystem on MSVC is
// read in the ANSI code page instead, so every file access goes through here.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

inline std::filesystem::path PathFromUtf8(const std::string& utf8)
{
  return std::filesystem::u8path(utf8);
}

inline bool ReadWholeFile(const std::filesystem::path& path, std::string& out)
{
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return !f.bad();
}

// Binary, so LF line endings survive on Windows.
inline bool WriteWholeFile(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return false;
  f.write(text.data(), (std::streamsize)text.size());
  f.close();
  return !f.fail();
}

inline bool ReadWholeFile(const std::string& utf8Path, std::string& out) { return ReadWholeFile(PathFromUtf8(utf8Path), out); }
inline bool WriteWholeFile(const std::string& utf8Path, const std::string& text) { return WriteWholeFile(PathFromUtf8(utf8Path), text); }
