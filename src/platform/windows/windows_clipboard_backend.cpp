#include "lanlink/platform/windows_clipboard_backend.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

namespace lanlink::platform {
namespace {
constexpr wchar_t kWindowClass[] = L"LANLink.ClipboardWindow";
constexpr wchar_t kWindowTitle[] = L"LANLink Clipboard";

std::string wide_to_utf8(const wchar_t* value, int length) {
  if (value == nullptr || length <= 0) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length, nullptr, 0,
                                        nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string result(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length, result.data(), bytes, nullptr,
                      nullptr);
  return result;
}

Result<std::wstring> utf8_to_wide(const std::string& value) {
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
  if (length < 0) return Result<std::wstring>::failure("clipboard text is not valid UTF-8");
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (length > 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                        static_cast<int>(value.size()), result.data(), length) == 0)
    return Result<std::wstring>::failure("clipboard text is not valid UTF-8");
  return Result<std::wstring>::success(std::move(result));
}

Result<std::vector<std::uint8_t>> read_global_bytes(HGLOBAL handle) {
  if (handle == nullptr) return Result<std::vector<std::uint8_t>>::failure("clipboard data is null");
  const auto size = GlobalSize(handle);
  void* locked = GlobalLock(handle);
  if (locked == nullptr) return Result<std::vector<std::uint8_t>>::failure("clipboard data lock failed");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0) std::memcpy(bytes.data(), locked, size);
  GlobalUnlock(handle);
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

HGLOBAL make_global_bytes(const void* data, std::size_t size) {
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, size == 0 ? 1 : size);
  if (handle == nullptr) return nullptr;
  void* locked = GlobalLock(handle);
  if (locked == nullptr) {
    GlobalFree(handle);
    return nullptr;
  }
  if (size > 0) std::memcpy(locked, data, size);
  GlobalUnlock(handle);
  return handle;
}

std::string html_clipboard_payload(const std::string& html) {
  constexpr std::string_view marker = "<!--StartFragment-->";
  constexpr std::string_view end_marker = "<!--EndFragment-->";
  std::string payload = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n";
  payload += "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
  const auto start_html = payload.size();
  payload += marker;
  const auto start_fragment = payload.size();
  payload += html;
  const auto end_fragment = payload.size();
  payload += end_marker;
  const auto end_html = payload.size();

  auto replace_offset = [&](std::string_view field, std::size_t value) {
    const auto position = payload.find(field);
    const auto digits = payload.find(':', position) + 1;
    const auto text = std::to_string(value);
    payload.replace(digits + (10 - text.size()), text.size(), text);
  };
  replace_offset("StartHTML:", start_html);
  replace_offset("EndHTML:", end_html);
  replace_offset("StartFragment:", start_fragment);
  replace_offset("EndFragment:", end_fragment);
  return payload;
}

std::string parse_html_clipboard_payload(const std::vector<std::uint8_t>& bytes) {
  const std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  const auto start_line = payload.find("StartHTML:");
  const auto end_line = payload.find("EndHTML:");
  if (start_line != std::string::npos && end_line != std::string::npos) {
    const auto start_end = payload.find('\r', start_line);
    const auto end_end = payload.find('\r', end_line);
    std::size_t start = 0;
    std::size_t end = 0;
    try {
      start = std::stoul(payload.substr(start_line + 10, start_end - start_line - 10));
      end = std::stoul(payload.substr(end_line + 8, end_end - end_line - 8));
    } catch (...) {
      return payload;
    }
    if (start <= end && end <= payload.size()) {
      auto fragment = payload.substr(start, end - start);
      constexpr std::string_view marker = "<!--StartFragment-->";
      constexpr std::string_view end_marker = "<!--EndFragment-->";
      const auto marker_pos = fragment.find(marker);
      const auto end_marker_pos = fragment.find(end_marker);
      if (marker_pos != std::string::npos && end_marker_pos > marker_pos)
        fragment = fragment.substr(marker_pos + marker.size(), end_marker_pos - marker_pos - marker.size());
      return fragment;
    }
  }
  return payload;
}
}  // namespace

struct WindowsClipboardBackend::Impl {
  explicit Impl(WindowsClipboardBackend* owner) : owner(owner) {}

  WindowsClipboardBackend* owner;
  std::thread thread;
  std::mutex mutex;
  std::condition_variable ready_condition;
  HWND window{nullptr};
  DWORD thread_id{0};
  UINT html_format{0};
  UINT png_format{0};
  bool ready{false};
  bool stop_requested{false};
  bool suppress_next{false};
  std::deque<ClipboardData> changes;

  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      impl = static_cast<Impl*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }
    if (impl != nullptr && message == WM_CLIPBOARDUPDATE) {
      std::lock_guard lock(impl->mutex);
      if (impl->suppress_next) {
        impl->suppress_next = false;
      } else {
        auto value = impl->read_clipboard();
        if (value.ok()) impl->changes.push_back(std::move(value.value()));
      }
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }

  Result<ClipboardData> read_clipboard() const {
    if (!OpenClipboard(window)) return Result<ClipboardData>::failure("failed to open clipboard");
    ClipboardData data;
    if (GetClipboardData(CF_UNICODETEXT) != nullptr) {
      const auto bytes = read_global_bytes(GetClipboardData(CF_UNICODETEXT));
      if (!bytes.ok()) {
        CloseClipboard();
        return Result<ClipboardData>::failure(bytes.error());
      }
      const auto* text = reinterpret_cast<const wchar_t*>(bytes.value().data());
      const auto chars = bytes.value().size() / sizeof(wchar_t);
      const auto length = chars > 0 && text[chars - 1] == L'\0' ? chars - 1 : chars;
      data.text = wide_to_utf8(text, static_cast<int>(length));
    }
    if (GetClipboardData(html_format) != nullptr) {
      const auto bytes = read_global_bytes(GetClipboardData(html_format));
      if (!bytes.ok()) {
        CloseClipboard();
        return Result<ClipboardData>::failure(bytes.error());
      }
      data.html = parse_html_clipboard_payload(bytes.value());
    }
    if (GetClipboardData(png_format) != nullptr) {
      const auto bytes = read_global_bytes(GetClipboardData(png_format));
      if (!bytes.ok()) {
        CloseClipboard();
        return Result<ClipboardData>::failure(bytes.error());
      }
      data.png = bytes.value();
    }
    CloseClipboard();
    const auto valid = validate_clipboard_data(data);
    if (!valid.ok()) return Result<ClipboardData>::failure(valid.error());
    return Result<ClipboardData>::success(std::move(data));
  }

  void thread_main() {
    thread_id = GetCurrentThreadId();
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* imaging_factory = nullptr;
    (void)CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&imaging_factory));

    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = &Impl::window_proc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kWindowClass;
    RegisterClassExW(&klass);
    window = CreateWindowExW(0, kWindowClass, kWindowTitle, 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, klass.hInstance, this);
    if (window != nullptr) {
      html_format = RegisterClipboardFormatW(L"HTML Format");
      png_format = RegisterClipboardFormatW(L"PNG");
      AddClipboardFormatListener(window);
    }
    {
      std::lock_guard lock(mutex);
      ready = true;
    }
    ready_condition.notify_all();

    MSG message{};
    while (window != nullptr && GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (window != nullptr) {
      RemoveClipboardFormatListener(window);
      DestroyWindow(window);
      window = nullptr;
    }
    if (imaging_factory != nullptr) imaging_factory->Release();
    CoUninitialize();
  }
};

WindowsClipboardBackend::WindowsClipboardBackend() : impl_(std::make_unique<Impl>(this)) {}
WindowsClipboardBackend::~WindowsClipboardBackend() { stop(); }

Result<void> WindowsClipboardBackend::start(ChangeCallback callback) {
  stop();
  impl_->owner = this;
  callback_ = std::move(callback);
  impl_->ready = false;
  impl_->thread = std::thread([this] { impl_->thread_main(); });
  std::unique_lock lock(impl_->mutex);
  impl_->ready_condition.wait(lock, [this] { return impl_->ready; });
  if (impl_->window == nullptr) {
    last_error_ = "failed to create clipboard window";
    return Result<void>::failure(last_error_);
  }
  last_error_.clear();
  return Result<void>::success();
}

void WindowsClipboardBackend::stop() {
  if (impl_->thread.joinable()) {
    if (impl_->thread_id != 0) PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    impl_->thread.join();
  }
  callback_ = {};
  impl_->thread_id = 0;
}

Result<void> WindowsClipboardBackend::poll() {
  if (!impl_->thread.joinable()) {
    last_error_ = "clipboard backend is not running";
    return Result<void>::failure(last_error_);
  }
  std::deque<ClipboardData> changes;
  {
    std::lock_guard lock(impl_->mutex);
    changes.swap(impl_->changes);
  }
  for (auto& change : changes)
    if (callback_) callback_(std::move(change));
  return Result<void>::success();
}

Result<ClipboardData> WindowsClipboardBackend::read() const {
  if (!impl_->thread.joinable()) return Result<ClipboardData>::failure("clipboard backend is not running");
  return impl_->read_clipboard();
}

Result<void> WindowsClipboardBackend::write(const ClipboardData& data) {
  const auto valid = validate_clipboard_data(data);
  if (!valid.ok()) {
    last_error_ = valid.error();
    return valid;
  }
  if (!impl_->thread.joinable() || impl_->window == nullptr) {
    last_error_ = "clipboard backend is not running";
    return Result<void>::failure(last_error_);
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->suppress_next = true;
  }
  if (!OpenClipboard(impl_->window)) {
    last_error_ = "failed to open clipboard";
    std::lock_guard lock(impl_->mutex);
    impl_->suppress_next = false;
    return Result<void>::failure(last_error_);
  }
  EmptyClipboard();
  if (data.text) {
    auto value = utf8_to_wide(*data.text);
    if (!value.ok()) {
      CloseClipboard();
      last_error_ = value.error();
      return Result<void>::failure(last_error_);
    }
    HGLOBAL handle = make_global_bytes(value.value().c_str(), (value.value().size() + 1) * sizeof(wchar_t));
    if (handle == nullptr || SetClipboardData(CF_UNICODETEXT, handle) == nullptr) {
      if (handle != nullptr) GlobalFree(handle);
      CloseClipboard();
      last_error_ = "failed to write clipboard text";
      return Result<void>::failure(last_error_);
    }
  }
  if (data.html) {
    const auto payload = html_clipboard_payload(*data.html);
    HGLOBAL handle = make_global_bytes(payload.data(), payload.size());
    if (handle == nullptr || SetClipboardData(impl_->html_format, handle) == nullptr) {
      if (handle != nullptr) GlobalFree(handle);
      CloseClipboard();
      last_error_ = "failed to write clipboard HTML";
      return Result<void>::failure(last_error_);
    }
  }
  if (data.png) {
    HGLOBAL handle = make_global_bytes(data.png->data(), data.png->size());
    if (handle == nullptr || SetClipboardData(impl_->png_format, handle) == nullptr) {
      if (handle != nullptr) GlobalFree(handle);
      CloseClipboard();
      last_error_ = "failed to write clipboard PNG";
      return Result<void>::failure(last_error_);
    }
  }
  CloseClipboard();
  last_error_.clear();
  return Result<void>::success();
}

Result<void> WindowsClipboardBackend::clear() { return write(ClipboardData{}); }

}  // namespace lanlink::platform

#else

namespace lanlink::platform {
struct WindowsClipboardBackend::Impl {};

WindowsClipboardBackend::WindowsClipboardBackend() : impl_(std::make_unique<Impl>()) {
  last_error_ = "Windows clipboard backend unsupported on this platform";
}
WindowsClipboardBackend::~WindowsClipboardBackend() = default;
Result<void> WindowsClipboardBackend::start(ChangeCallback) {
  return Result<void>::failure(last_error_);
}
void WindowsClipboardBackend::stop() {}
Result<void> WindowsClipboardBackend::poll() { return Result<void>::failure(last_error_); }
Result<ClipboardData> WindowsClipboardBackend::read() const {
  return Result<ClipboardData>::failure(last_error_);
}
Result<void> WindowsClipboardBackend::write(const ClipboardData&) {
  return Result<void>::failure(last_error_);
}
Result<void> WindowsClipboardBackend::clear() { return Result<void>::failure(last_error_); }
}  // namespace lanlink::platform

#endif
