#include "lanlink/platform/macos_clipboard_backend.hpp"

#include <utility>

#if defined(__APPLE__)

#include <AppKit/AppKit.h>

namespace lanlink::platform {
namespace {
constexpr const char* kHtmlType = "public.html";

Result<NSString*> to_ns_string(const std::string& value) {
  NSString* string = [[NSString alloc] initWithBytes:value.data()
                                               length:value.size()
                                             encoding:NSUTF8StringEncoding];
  if (string == nil) return Result<NSString*>::failure("clipboard text is not valid UTF-8");
  return Result<NSString*>::success(string);
}

std::optional<std::string> from_ns_string(NSString* value) {
  if (value == nil) return std::nullopt;
  const char* utf8 = [value UTF8String];
  return utf8 == nullptr ? std::optional<std::string>{} : std::optional<std::string>{utf8};
}
}  // namespace

struct MacOSClipboardBackend::Impl {
  NSPasteboard* pasteboard{[NSPasteboard generalPasteboard]};
  NSInteger change_count{0};
};

MacOSClipboardBackend::MacOSClipboardBackend() : impl_(std::make_unique<Impl>()) {}
MacOSClipboardBackend::~MacOSClipboardBackend() { stop(); }

Result<void> MacOSClipboardBackend::start(ChangeCallback callback) {
  @autoreleasepool {
    callback_ = std::move(callback);
    running_ = true;
    impl_->change_count = [impl_->pasteboard changeCount];
    last_error_.clear();
    return Result<void>::success();
  }
}

void MacOSClipboardBackend::stop() {
  running_ = false;
  callback_ = {};
}

Result<ClipboardData> MacOSClipboardBackend::read() const {
  @autoreleasepool {
    ClipboardData data;
    NSPasteboard* pasteboard = impl_->pasteboard;
    data.text = from_ns_string([pasteboard stringForType:NSPasteboardTypeString]);

    NSData* html = [pasteboard dataForType:[NSString stringWithUTF8String:kHtmlType]];
    if (html != nil)
      data.html = std::string(static_cast<const char*>([html bytes]), [html length]);

    NSData* png = [pasteboard dataForType:NSPasteboardTypePNG];
    if (png != nil) {
      const auto* bytes = static_cast<const std::uint8_t*>([png bytes]);
      data.png = std::vector<std::uint8_t>(bytes, bytes + [png length]);
    }
    const auto valid = validate_clipboard_data(data);
    if (!valid.ok()) return Result<ClipboardData>::failure(valid.error());
    return Result<ClipboardData>::success(std::move(data));
  }
}

Result<void> MacOSClipboardBackend::poll() {
  @autoreleasepool {
    if (!running_) {
      last_error_ = "clipboard backend is not running";
      return Result<void>::failure(last_error_);
    }
    const NSInteger current = [impl_->pasteboard changeCount];
    if (current == impl_->change_count) return Result<void>::success();
    impl_->change_count = current;
    auto result = read();
    if (!result.ok()) {
      last_error_ = result.error();
      return Result<void>::failure(last_error_);
    }
    if (callback_) callback_(std::move(result.value()));
    return Result<void>::success();
  }
}

Result<void> MacOSClipboardBackend::write(const ClipboardData& data) {
  @autoreleasepool {
    const auto valid = validate_clipboard_data(data);
    if (!valid.ok()) {
      last_error_ = valid.error();
      return valid;
    }
    if (!running_) {
      last_error_ = "clipboard backend is not running";
      return Result<void>::failure(last_error_);
    }

    NSPasteboard* pasteboard = impl_->pasteboard;
    [pasteboard clearContents];
    NSMutableArray<NSString*>* types = [NSMutableArray arrayWithCapacity:3];
    if (data.text) [types addObject:NSPasteboardTypeString];
    if (data.html) [types addObject:[NSString stringWithUTF8String:kHtmlType]];
    if (data.png) [types addObject:NSPasteboardTypePNG];
    if ([types count] > 0 && [pasteboard declareTypes:types owner:nil] == 0) {
      last_error_ = "failed to declare clipboard types";
      return Result<void>::failure(last_error_);
    }

    if (data.text) {
      auto value = to_ns_string(*data.text);
      if (!value.ok() || ![pasteboard setString:value.value() forType:NSPasteboardTypeString]) {
        last_error_ = value.ok() ? "failed to write clipboard text" : value.error();
        return Result<void>::failure(last_error_);
      }
    }
    if (data.html) {
      auto value = to_ns_string(*data.html);
      if (!value.ok() || ![pasteboard setString:value.value()
                                      forType:[NSString stringWithUTF8String:kHtmlType]]) {
        last_error_ = value.ok() ? "failed to write clipboard HTML" : value.error();
        return Result<void>::failure(last_error_);
      }
    }
    if (data.png) {
      NSData* value = [NSData dataWithBytes:data.png->data() length:data.png->size()];
      if (![pasteboard setData:value forType:NSPasteboardTypePNG]) {
        last_error_ = "failed to write clipboard PNG";
        return Result<void>::failure(last_error_);
      }
    }
    impl_->change_count = [pasteboard changeCount];
    last_error_.clear();
    return Result<void>::success();
  }
}

Result<void> MacOSClipboardBackend::clear() { return write(ClipboardData{}); }

}  // namespace lanlink::platform

#else

namespace lanlink::platform {
struct MacOSClipboardBackend::Impl {};

MacOSClipboardBackend::MacOSClipboardBackend() : impl_(std::make_unique<Impl>()) {
  last_error_ = "macOS clipboard backend unsupported on this platform";
}
MacOSClipboardBackend::~MacOSClipboardBackend() = default;

Result<void> MacOSClipboardBackend::start(ChangeCallback) {
  last_error_ = "macOS clipboard backend unsupported on this platform";
  return Result<void>::failure(last_error_);
}
void MacOSClipboardBackend::stop() {
  running_ = false;
  callback_ = {};
}
Result<void> MacOSClipboardBackend::poll() {
  return Result<void>::failure(last_error_);
}
Result<ClipboardData> MacOSClipboardBackend::read() const {
  return Result<ClipboardData>::failure(last_error_);
}
Result<void> MacOSClipboardBackend::write(const ClipboardData&) {
  return Result<void>::failure(last_error_);
}
Result<void> MacOSClipboardBackend::clear() {
  return Result<void>::failure(last_error_);
}
}  // namespace lanlink::platform

#endif
