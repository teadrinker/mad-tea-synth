
#include "ExportTemplate.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Generated into the binary dir; never a stale copy beside this file.
#include "export_template_embedded.h"

namespace fs = std::filesystem;

const ExportTemplateFile* ExportTemplateAll(size_t& count)
{
  count = sizeof(kExportTemplateFiles) / sizeof(kExportTemplateFiles[0]);
  return kExportTemplateFiles;
}

std::string ExportTemplateRead(const ExportTemplateFile& f)
{
  if (f.bytes) return std::string((const char*)f.bytes, f.byteCount);

  size_t total = 0;
  for (const char* const* c = f.chunks; *c; ++c) total += std::char_traits<char>::length(*c);

  std::string out;
  out.reserve(total);
  for (const char* const* c = f.chunks; *c; ++c) out += *c;
  return out;
}

namespace {
// A name with no value is an error. One pass, so a value containing `{{` is
// never substituted again.
bool Substitute(const std::string& in, const std::vector<ExportTemplateVar>& vars,
                const char* path, std::string& out, std::string& err)
{
  out.clear();
  out.reserve(in.size());

  for (size_t i = 0; i < in.size(); )
  {
    if (in[i] != '{' || i + 1 >= in.size() || in[i + 1] != '{') { out += in[i++]; continue; }

    const size_t close = in.find("}}", i + 2);
    if (close == std::string::npos)
    {
      err = std::string("'") + path + "': unterminated {{ at offset "
          + std::to_string(i);
      return false;
    }

    const std::string name = in.substr(i + 2, close - (i + 2));
    const ExportTemplateVar* hit = nullptr;
    for (const ExportTemplateVar& v : vars)
      if (name == v.name) { hit = &v; break; }

    if (!hit)
    {
      err = std::string("'") + path + "': no value for {{" + name + "}}";
      return false;
    }

    out += hit->value;
    i = close + 2;
  }
  return true;
}
} // namespace

bool ExportTemplateWrite(unsigned target, const std::string& destDir,
                         int& filesWritten, std::string& err,
                         const std::vector<ExportTemplateVar>& vars,
                         ExportTemplateMode mode)
{
  filesWritten = 0;
  err.clear();

  fs::path base = fs::u8path(destDir);

  size_t count = 0;
  const ExportTemplateFile* files = ExportTemplateAll(count);

  for (size_t i = 0; i < count; ++i)
  {
    const ExportTemplateFile& f = files[i];
    if ((f.targets & target) == 0) continue;

    fs::path out = base / fs::u8path(std::string(f.path));

    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    if (ec) { err = std::string("cannot create directory for '") + f.path + "': " + ec.message(); return false; }

    // Binary: the text already has the repo's newlines, and the tree is
    // byte-compared against the repo.
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o.is_open()) { err = std::string("cannot open '") + f.path + "' for writing"; return false; }

    std::string text = ExportTemplateRead(f);
    if (f.subst && mode == kExportTemplateFill)
    {
      std::string filled;
      if (!Substitute(text, vars, f.path, filled, err)) return false;
      text.swap(filled);
    }
    o.write(text.data(), (std::streamsize)text.size());
    if (!o.good()) { err = std::string("write to '") + f.path + "' failed"; return false; }
    o.close();

    ++filesWritten;
  }

  if (filesWritten == 0) { err = "no template files match that target"; return false; }
  return true;
}
