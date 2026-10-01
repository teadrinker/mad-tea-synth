// Native folder dialog for the export page; iPlug2's PromptForFile is file-only.
// Windows only; elsewhere the path is typed.

#include <string>

#ifdef _WIN32

#include <windows.h>
#include <shobjidl.h>

// IFileDialog, which accepts a pasted path.
bool PlatformFolderPicker_Choose(const std::string& initialUtf8, std::string& outUtf8)
{
  outUtf8.clear();

  // The host owns COM's threading model; on RPC_E_CHANGED_MODE don't uninitialise.
  HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool weInitialised = SUCCEEDED(hrInit);

  bool ok = false;
  IFileDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dialog))))
  {
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
      dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    if (!initialUtf8.empty())
    {
      int n = MultiByteToWideChar(CP_UTF8, 0, initialUtf8.data(), (int)initialUtf8.size(), nullptr, 0);
      if (n > 0)
      {
        std::wstring wide((size_t)n, 0);
        MultiByteToWideChar(CP_UTF8, 0, initialUtf8.data(), (int)initialUtf8.size(), &wide[0], n);
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(&item))))
        {
          dialog->SetFolder(item);
          item->Release();
        }
      }
    }

    if (SUCCEEDED(dialog->Show(GetActiveWindow())))
    {
      IShellItem* result = nullptr;
      if (SUCCEEDED(dialog->GetResult(&result)))
      {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
        {
          int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
          if (n > 1)
          {
            outUtf8.resize((size_t)n - 1);
            WideCharToMultiByte(CP_UTF8, 0, path, -1, &outUtf8[0], n, nullptr, nullptr);
            ok = true;
          }
          CoTaskMemFree(path);
        }
        result->Release();
      }
    }
    dialog->Release();
  }

  if (weInitialised) CoUninitialize();
  return ok;
}

#else

bool PlatformFolderPicker_Choose(const std::string& initialUtf8, std::string& outUtf8)
{
  (void)initialUtf8;
  outUtf8.clear();
  return false; // type the path instead -- the settings field accepts one
}

#endif
