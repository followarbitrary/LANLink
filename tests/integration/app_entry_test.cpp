#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string quote_shell(const std::string& value) {
#if defined(_WIN32)
  std::string quoted{"\""};
  for (const char character : value) {
    if (character == '"') quoted += '\\';
    quoted += character;
  }
  quoted += '"';
  return quoted;
#else
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
#endif
}

int run_case(const std::string& executable, const std::string& arguments,
             bool expect_success, const std::string& expected_output) {
  static std::atomic<unsigned long long> sequence{0};
  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto unique_sequence = sequence.fetch_add(1, std::memory_order_relaxed);
  const auto output_path = std::filesystem::temp_directory_path() /
                           ("lanlink_app_entry_test_" +
                            std::to_string(static_cast<unsigned long long>(timestamp)) + "_" +
                            std::to_string(unique_sequence) +
                            ".txt");
  const std::string command = quote_shell(executable) + " " + arguments + " > " +
                              quote_shell(output_path.string()) + " 2>&1";
  const int status = std::system(command.c_str());

  std::ifstream output_file(output_path);
  const std::string output((std::istreambuf_iterator<char>(output_file)),
                           std::istreambuf_iterator<char>());
  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);

  const bool succeeded = status == 0;
  if (succeeded != expect_success) {
    std::cerr << "unexpected exit status " << status << "\n" << output;
    return 1;
  }
  if (output.find(expected_output) == std::string::npos) {
    std::cerr << "expected output containing " << expected_output << "\n" << output;
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: app_entry_test <lanlink_app> <help|invalid-port>\n";
    return 2;
  }
  const std::string executable = argv[1];
  const std::string mode = argv[2];
  if (mode == "help") return run_case(executable, "--help", true, "--connect");
  if (mode == "invalid-port") {
    return run_case(executable,
                    "--device-id 1 --peer-id 2 --listen-port 0",
                    false, "--listen-port");
  }
  std::cerr << "unknown smoke-test mode: " << mode << '\n';
  return 2;
}
