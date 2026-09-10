// Windows Toast notification helper (WinRT), shipped as a standalone FFI DLL.
//
// Loaded dynamically by app/lib/util/native/windows_notification.dart via
// dart:ffi (no MethodChannel, no modification of the Flutter runner).
// Pure additive: this DLL is not referenced by the official executable.
//
// Design notes (see .docs/2026-09-09_Windows弹窗通知模块):
// - Toast activation is NOT handled via COM here: clicking a toast launches
//   the shortcut target (the LocalSend exe); the app's existing single
//   instance handshake brings the window to front and Dart routes the action.
// - Unpackaged (non-MSIX) apps need a Start Menu shortcut carrying an
//   AppUserModelID before ToastNotificationManager can create a notifier.
//
// Debugging: every call writes a line to %TEMP%\localsend_windows_notify.log.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

#include <propkey.h>
#include <propsys.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kLnkFileName[] = L"LocalSend Notification.lnk";
constexpr wchar_t kLnkName[] = L"LocalSend Notification";

using LsActivationCallback = void(__cdecl*)(const wchar_t*);

/// Registered by the Dart side via ls_toast_set_activation_callback.
LsActivationCallback g_activation_callback = nullptr;

/// Live toast objects, keyed by tag. They are kept alive (and thereby stay
/// subscribed to Activated) for the lifetime of the process. A toast shown by a
/// previous process cannot be observed from here anyway.
std::map<std::wstring, winrt::Windows::UI::Notifications::ToastNotification> g_live_toasts;

/// Guards g_activation_callback and g_live_toasts (touched from several threads).
std::mutex g_mutex;

void NotifyLog(const wchar_t* format, ...) {
  wchar_t buf[1024];
  va_list args;
  va_start(args, format);
  vswprintf_s(buf, 1024, format, args);
  va_end(args);
  wchar_t temp_dir[MAX_PATH]{};
  if (GetTempPathW(MAX_PATH, temp_dir) == 0) {
    return;
  }
  std::wstring path(temp_dir);
  if (!path.empty() && path.back() != L'\\') {
    path += L'\\';
  }
  path += L"localsend_windows_notify.log";
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f != nullptr) {
    fwprintf(f, L"%s\n", buf);
    fclose(f);
  }
}

/// Brings one of our own top-level windows to the foreground. Native fallback so
/// that a clicked "show the panel" toast never depends on Dart timing.
void FocusOwnWindow() {
  struct Search {
    DWORD pid;
    HWND found;
  } search{GetCurrentProcessId(), nullptr};

  EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        auto* s = reinterpret_cast<Search*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != s->pid || GetWindow(hwnd, GW_OWNER) != nullptr) {
          return TRUE;
        }
        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, 256);
        if (title[0] == L'\0') {
          return TRUE;  // skip helper/tool windows
        }
        s->found = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&search));

  if (search.found == nullptr) {
    NotifyLog(L"focus: no window found");
    return;
  }
  HWND hwnd = search.found;
  if (IsIconic(hwnd)) {
    ShowWindow(hwnd, SW_RESTORE);
  } else {
    ShowWindow(hwnd, SW_SHOW);
  }
  const HWND foreground = GetForegroundWindow();
  const DWORD foreground_thread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
  const DWORD current_thread = GetCurrentThreadId();
  bool attached = false;
  if (foreground_thread != 0 && foreground_thread != current_thread) {
    attached = AttachThreadInput(foreground_thread, current_thread, TRUE) != 0;
  }
  SetForegroundWindow(hwnd);
  BringWindowToTop(hwnd);
  if (attached) {
    AttachThreadInput(foreground_thread, current_thread, FALSE);
  }
  NotifyLog(L"focus: hwnd=%p done", hwnd);
}

/// Toasts are created on one long-lived MTA thread. Creating them on throwaway
/// threads (one per Dart `Isolate.run` call) lets the COM apartment be torn down
/// while the toast is still alive, which silently kills its Activated
/// subscription - that is why some notifications could not be clicked.
std::mutex g_worker_mutex;
std::condition_variable g_worker_cv;
std::deque<std::function<void()>> g_worker_queue;

void EnsureToastWorker() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      NotifyLog(L"toast worker started");
      for (;;) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(g_worker_mutex);
          g_worker_cv.wait(lock, [] { return !g_worker_queue.empty(); });
          task = std::move(g_worker_queue.front());
          g_worker_queue.pop_front();
        }
        try {
          task();
        } catch (...) {
          NotifyLog(L"toast worker: task threw");
        }
      }
    }).detach();
  });
}

/// Runs [task] on the toast worker thread and returns its result.
int RunOnToastWorker(std::function<int()> task) {
  EnsureToastWorker();
  auto promise = std::make_shared<std::promise<int>>();
  auto future = promise->get_future();
  {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    g_worker_queue.push_back([task = std::move(task), promise]() { promise->set_value(task()); });
  }
  g_worker_cv.notify_one();
  return future.get();
}

std::wstring XmlEscape(const std::wstring& in) {
  std::wstring out;
  out.reserve(in.size() + 16);
  for (const wchar_t c : in) {
    switch (c) {
      case L'&': out += L"&amp;"; break;
      case L'<': out += L"&lt;"; break;
      case L'>': out += L"&gt;"; break;
      case L'"': out += L"&quot;"; break;
      case L'\'': out += L"&apos;"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::wstring StartMenuLnkPath() {
  PWSTR programs = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
    return std::wstring();
  }
  std::wstring path(programs);
  CoTaskMemFree(programs);
  if (!path.empty() && path.back() != L'\\') {
    path += L'\\';
  }
  path += kLnkFileName;
  return path;
}

/// Shows the toast on the worker thread (which owns the MTA apartment). Toast
/// objects are kept alive in g_live_toasts so their Activated subscription
/// survives until the process exits.
int ShowToast(const std::wstring& app_id, const std::wstring& title,
              const std::wstring& body, const std::wstring& tag,
              const std::wstring& launch) {
  NotifyLog(L"show(app_id=%s, title=%s, tag=%s)", app_id.c_str(), title.c_str(), tag.c_str());
  if (app_id.empty() || title.empty()) {
    NotifyLog(L"show: invalid args");
    return -1;
  }
  try {
    std::wstring xml = L"<toast";
    if (!launch.empty()) {
      // Payload round-tripped to the activation callback on click.
      xml += L" launch=\"" + XmlEscape(launch) + L"\"";
    }
    xml += L"><visual><binding template=\"ToastGeneric\"><text>" +
           XmlEscape(title) + L"</text><text>" + XmlEscape(body) +
           L"</text></binding></visual></toast>";

    winrt::Windows::Data::Xml::Dom::XmlDocument doc;
    doc.LoadXml(xml);
    winrt::Windows::UI::Notifications::ToastNotification toast(doc);
    if (!tag.empty()) {
      toast.Tag(winrt::hstring(tag));
    }

    // Subscribe before showing: while our process runs, a click must reach us
    // (with its payload) instead of silently relaunching the executable.
    toast.Activated([launch](const winrt::Windows::UI::Notifications::ToastNotification&,
                             const winrt::Windows::Foundation::IInspectable&) {
      NotifyLog(L"activated: launch=%s", launch.c_str());
      // Native fallback: reveal the panel right here instead of relying on Dart
      // being able to take the foreground.
      if (launch.rfind(L"action=home", 0) == 0) {
        FocusOwnWindow();
      }
      LsActivationCallback callback = nullptr;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        callback = g_activation_callback;
      }
      if (callback != nullptr) {
        callback(launch.c_str());
      } else {
        NotifyLog(L"activated: no Dart callback registered, ignored");
      }
    });

    // Keep the object alive so the Activated subscription stays valid.
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      const std::wstring slot = !tag.empty() ? tag : std::wstring(L"__default__");
      g_live_toasts.insert_or_assign(slot, toast);
    }

    try {
      auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(
          winrt::hstring(app_id));
      NotifyLog(L"show: notifier created, calling Show()");
      notifier.Show(toast);
      NotifyLog(L"show: Show() returned without exception");
      return 0;
    } catch (winrt::hresult_error const& e) {
      NotifyLog(L"show: notifier/Show hr=0x%08X", e.code());
      return -(200 + (e.code() & 0x7FFF));  // ~ -20105 = E_ILLEGAL_METHOD_CALL etc.
    } catch (...) {
      NotifyLog(L"show: notifier/Show unknown exception");
      return -2;
    }
  } catch (winrt::hresult_error const& e) {
    NotifyLog(L"show: WinRT hr=0x%08X", e.code());
    return -(100 + (e.code() & 0x7FFF));  // carries HRESULT low bits for debugging.
  } catch (...) {
    NotifyLog(L"show: unknown exception");
    return -3;
  }
}

}  // namespace

extern "C" {

// Returns 1 if the current Windows build supports toast notifications (>= 10.0.10240).
__declspec(dllexport) int ls_toast_supported(void) noexcept {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    NotifyLog(L"supported: ntdll missing");
    return 0;
  }
  typedef LONG(WINAPI* RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
  auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
  if (fn == nullptr) {
    NotifyLog(L"supported: RtlGetVersion missing");
    return 0;
  }
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (fn(&info) != 0) {
    return 0;
  }
  const int supported = (info.dwMajorVersion > 10 || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 10240)) ? 1 : 0;
  NotifyLog(L"supported=%d (win %u.%u.%u)", supported, info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
  return supported;
}

// Registers a Start Menu shortcut for the current executable carrying the
// given AppUserModelID, so that unpackaged toast notifications can be shown
// and their click can activate the app. Re-runs overwrite the shortcut.
// Returns 0 on success, non-zero on failure.
__declspec(dllexport) int ls_toast_ensure_identity(const wchar_t* app_id) noexcept {
  NotifyLog(L"ensure_identity(app_id=%s)", app_id != nullptr ? app_id : L"(null)");
  if (app_id == nullptr || app_id[0] == L'\0') {
    NotifyLog(L"ensure_identity: bad app_id");
    return -1;
  }
  const std::wstring lnk_path = StartMenuLnkPath();
  if (lnk_path.empty()) {
    NotifyLog(L"ensure_identity: cannot resolve Start Menu path");
    return -2;
  }
  NotifyLog(L"ensure_identity: lnk=%s", lnk_path.c_str());

  wchar_t exe_path[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
    NotifyLog(L"ensure_identity: GetModuleFileName failed");
    return -3;
  }
  NotifyLog(L"ensure_identity: exe=%s", exe_path);

  winrt::com_ptr<IShellLinkW> link;
  HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.put()));
  if (FAILED(hr)) {
    NotifyLog(L"ensure_identity: CoCreate ShellLink hr=0x%08X", hr);
    return -4;
  }
  if (FAILED(link->SetPath(exe_path))) {
    NotifyLog(L"ensure_identity: SetPath failed");
    return -5;
  }
  if (FAILED(link->SetDescription(kLnkName))) {
    NotifyLog(L"ensure_identity: SetDescription failed");
    return -6;
  }
  // Use the executable's own icon (LocalSend logo) for the shortcut; unpackaged
  // toast notifications display the shortcut icon as the app icon.
  if (FAILED(link->SetIconLocation(exe_path, 0))) {
    NotifyLog(L"ensure_identity: SetIconLocation failed");
    return -7;
  }
  auto persist = link.as<IPersistFile>();
  hr = persist->Save(lnk_path.c_str(), TRUE);
  if (FAILED(hr)) {
    NotifyLog(L"ensure_identity: Save lnk hr=0x%08X", hr);
    return -8;
  }

  winrt::com_ptr<IShellItem2> shell_item;
  hr = SHCreateItemFromParsingName(lnk_path.c_str(), nullptr, IID_PPV_ARGS(shell_item.put()));
  if (FAILED(hr)) {
    NotifyLog(L"ensure_identity: SHCreateItem hr=0x%08X", hr);
    return -9;
  }
  winrt::com_ptr<IPropertyStore> store;
  hr = shell_item->GetPropertyStore(GPS_READWRITE, IID_PPV_ARGS(store.put()));
  if (FAILED(hr)) {
    NotifyLog(L"ensure_identity: GetPropertyStore hr=0x%08X", hr);
    return -10;
  }
  PROPVARIANT value{};
  value.vt = VT_LPWSTR;
  value.pwszVal = const_cast<wchar_t*>(app_id);
  hr = store->SetValue(PKEY_AppUserModel_ID, value);
  if (FAILED(hr)) {
    NotifyLog(L"ensure_identity: SetValue hr=0x%08X", hr);
    return -11;
  }
  hr = store->Commit();
  if (FAILED(hr)) {
    NotifyLog(L"ensure_identity: Commit hr=0x%08X", hr);
    return -12;
  }

  // Read back to confirm the AUMID really persisted on the shortcut.
  winrt::com_ptr<IPropertyStore> verify_store;
  if (SUCCEEDED(shell_item->GetPropertyStore(GPS_READWRITE, IID_PPV_ARGS(verify_store.put())))) {
    PROPVARIANT got{};
    if (SUCCEEDED(verify_store->GetValue(PKEY_AppUserModel_ID, &got)) && got.vt == VT_LPWSTR) {
      NotifyLog(L"ensure_identity: AUMID readback '%s' (expect '%s')",
                got.pwszVal != nullptr ? got.pwszVal : L"", app_id);
      PropVariantClear(&got);
    } else {
      NotifyLog(L"ensure_identity: AUMID readback MISSING");
    }
  } else {
    NotifyLog(L"ensure_identity: AUMID verify store unavailable");
  }

  // Desktop (unpackaged) apps must declare the explicit AppUserModelID for the
  // current process; otherwise toast notifications are silently not shown.
  hr = SetCurrentProcessExplicitAppUserModelID(app_id);
  NotifyLog(L"ensure_identity: SetCurrentProcessExplicitAppUserModelID hr=0x%08X", hr);

  // Register a display name for the AUMID so the toast header shows "LocalSend"
  // instead of the shortcut file name ("LocalSend Notification").
  HKEY aumid_key = nullptr;
  const std::wstring aumid_reg_path = L"Software\\Classes\\AppUserModelId\\" + std::wstring(app_id);
  LSTATUS rs = RegCreateKeyExW(HKEY_CURRENT_USER, aumid_reg_path.c_str(), 0, nullptr, 0,
                               KEY_SET_VALUE, nullptr, &aumid_key, nullptr);
  if (rs == ERROR_SUCCESS && aumid_key != nullptr) {
    const wchar_t display_name[] = L"LocalSend";
    rs = RegSetValueExW(aumid_key, L"DisplayName", 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(display_name),
                        static_cast<DWORD>((wcslen(display_name) + 1) * sizeof(wchar_t)));
    RegCloseKey(aumid_key);
    NotifyLog(L"ensure_identity: AUMID DisplayName rs=%ld", static_cast<long>(rs));
  } else {
    NotifyLog(L"ensure_identity: AUMID registry create failed rs=%ld", static_cast<long>(rs));
  }

  return 0;
}

// Shows a toast notification with the given title/body. `tag` and `launch` may
// be nullptr. `launch` is handed back verbatim when the user clicks the toast
// (see ls_toast_set_activation_callback), so it carries the click action.
// The work runs on the long-lived toast worker thread - see ShowToast for the
// result codes (0 = ok, -1 = invalid args, -2/-3 = WinRT failures,
// -(100/200 + low HRESULT bits) on exceptions).
__declspec(dllexport) int ls_toast_show(const wchar_t* app_id, const wchar_t* title,
                                        const wchar_t* body, const wchar_t* tag,
                                        const wchar_t* launch) noexcept {
  // Copy everything first: the caller's buffers are not valid on the worker.
  return RunOnToastWorker([app_id_str = std::wstring(app_id != nullptr ? app_id : L""),
                           title_str = std::wstring(title != nullptr ? title : L""),
                           body_str = std::wstring(body != nullptr ? body : L""),
                           tag_str = std::wstring(tag != nullptr ? tag : L""),
                           launch_str = std::wstring(launch != nullptr ? launch : L"")]() {
    return ShowToast(app_id_str, title_str, body_str, tag_str, launch_str);
  });
}

// Registers the Dart callback for toast clicks (nullptr clears it). The
// callback runs on a WinRT thread and receives the toast's `launch` payload.
// Returns 0 always.
__declspec(dllexport) int ls_toast_set_activation_callback(LsActivationCallback callback) noexcept {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_activation_callback = callback;
  }
  NotifyLog(L"set_activation_callback: %s", callback != nullptr ? L"registered" : L"cleared");
  return 0;
}

}  // extern "C"
