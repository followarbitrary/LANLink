#pragma once
#include <string>
#include <utility>
#include <variant>

namespace lanlink {
template <typename T>
class Result {
 public:
  static Result success(T value) { return Result(std::move(value)); }
  static Result failure(std::string error) { return Result(Error{std::move(error)}); }
  bool ok() const noexcept { return std::holds_alternative<T>(data_); }
  bool has_value() const noexcept { return ok(); }
  explicit operator bool() const noexcept { return ok(); }
  const T& value() const { return std::get<T>(data_); }
  T& value() { return std::get<T>(data_); }
  const std::string& error() const { return std::get<Error>(data_).message; }
 private:
  struct Error { std::string message; };
  explicit Result(T value) : data_(std::move(value)) {}
  explicit Result(Error error) : data_(std::move(error)) {}
  std::variant<T, Error> data_;
};

template <>
class Result<void> {
 public:
  static Result success() { return Result(true, {}); }
  static Result failure(std::string error) { return Result(false, std::move(error)); }
  bool ok() const noexcept { return success_; }
  bool has_value() const noexcept { return success_; }
  explicit operator bool() const noexcept { return success_; }
  const std::string& error() const noexcept { return error_; }
 private:
  Result(bool success, std::string error) : success_(success), error_(std::move(error)) {}
  bool success_;
  std::string error_;
};
}
