// Copyright 2011-2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/flags/flag.h"
#include "third_party/absl/flags/parse.h"
#include "third_party/absl/flags/usage.h"
#include "third_party/absl/flags/usage_config.h"
#include "third_party/absl/status/status.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/absl/strings/str_split.h"
#include "third_party/zynamics/bindiff/config.h"
#include "third_party/zynamics/bindiff/version.h"
#include "third_party/zynamics/binexport/util/filesystem.h"
#include "third_party/zynamics/binexport/util/process.h"
#include "third_party/zynamics/binexport/util/status_macros.h"

ABSL_FLAG(std::string, config, "", "Config file name to use. Required");
ABSL_FLAG(bool, print_only, false,
          "Print final configuration to stdout and exit");
ABSL_FLAG(bool, help_settings, false,
          "Print the list of settings this tool can modify and exit");
ABSL_FLAG(bool, per_user, false,
          "Perform per-user setup of links to disassembler plugins");

namespace security::bindiff {

using ::security::binexport::GetLastOsError;
using ::security::binexport::GetOrCreateAppDataDirectory;

absl::StatusOr<std::string> GetOrCreateIdaProUserPluginsDirectory() {
  std::string idapro_app_data;
#if defined(_WIN32)
  constexpr absl::string_view kIdaPro = R"(Hex-Rays\IDA Pro)";
  NA_ASSIGN_OR_RETURN(idapro_app_data, GetOrCreateAppDataDirectory(kIdaPro));
#elif defined(__APPLE__)
  // On macOS, IDA Pro stores its settings directly in the user's home folder
  // under ".idapro" instead of "Library/Application Support/idapro", which
  // is what GetOrCreateAppDataDirectory() would produce.
  constexpr absl::string_view kIdaPro = ".idapro";
  const char* home_dir = getenv("HOME");
  if (!home_dir) {
    return absl::NotFoundError("Home directory not set");
  }
  idapro_app_data = JoinPath(home_dir, kIdaPro);
#else
  constexpr absl::string_view kIdaPro = "idapro";
  NA_ASSIGN_OR_RETURN(idapro_app_data, GetOrCreateAppDataDirectory(kIdaPro));
#endif
  std::string idapro_app_data_plugin_path =
      JoinPath(idapro_app_data, "plugins");
  NA_RETURN_IF_ERROR(CreateDirectories(idapro_app_data_plugin_path));
  return idapro_app_data_plugin_path;
}

// Sets up per-user configuration, creating links to the disassembler plugins.
// On Linux and macOS, always creates symlinks. On Windows, tries to create
// symlinks first, falling back to hardlinks and copying the files as a last
// resort.
absl::Status PerUserSetup(const Config& config) {
  const std::string& bindiff_dir = config.directory();
  if (bindiff_dir.empty()) {
    return absl::FailedPreconditionError(
        "Path to BinDiff missing from config file");
  }

#if defined(_WIN32)
  constexpr absl::string_view kLibrarySuffix = ".dll";

  constexpr absl::string_view kBinaryNinja = "Binary Ninja";
  constexpr absl::string_view kBinDiffBinaryNinjaPluginsPrefix =
      R"(Plugins\Binary Ninja)";

  constexpr absl::string_view kBinDiffIdaProPluginsPrefix =
      R"(Plugins\IDA Pro)";
#elif defined(__APPLE__)
  constexpr absl::string_view kLibrarySuffix = ".dylib";

  constexpr absl::string_view kBinaryNinja = "Binary Ninja";
  constexpr absl::string_view kBinDiffBinaryNinjaPluginsPrefix =
      "../../../Plugins/Binary Ninja";  // Relative to .app bundle

  constexpr absl::string_view kBinDiffIdaProPluginsPrefix =
      "../../../Plugins/IDA Pro";  // Relative to .app bundle
#else
  constexpr absl::string_view kLibrarySuffix = ".so";

  constexpr absl::string_view kBinaryNinja = "binaryninja";
  constexpr absl::string_view kBinDiffBinaryNinjaPluginsPrefix =
      "plugins/binaryninja";

  constexpr absl::string_view kBinDiffIdaProPluginsPrefix = "plugins/idapro";
#endif

  // Binary Ninja
  NA_ASSIGN_OR_RETURN(const std::string binaryninja_app_data,
                      GetOrCreateAppDataDirectory(kBinaryNinja));
  const std::string binaryninja_app_data_plugin_path =
      JoinPath(binaryninja_app_data, "plugins");
  NA_RETURN_IF_ERROR(CreateDirectories(binaryninja_app_data_plugin_path));

  std::string plugin_basename = absl::StrFormat(
      "binexport%s_binaryninja%s", kBinDiffBinExportRelease, kLibrarySuffix);
  if (auto status = CreateOrUpdateLinkWithFallback(
          JoinPath(bindiff_dir, kBinDiffBinaryNinjaPluginsPrefix,
                   plugin_basename),
          JoinPath(binaryninja_app_data_plugin_path, plugin_basename));
      // Binary Ninja may not have been selected during install, so skip if not
      // found
      !status.ok() && !absl::IsNotFound(status)) {
    return status;
  }

  // IDA Pro
  NA_ASSIGN_OR_RETURN(const std::string idapro_app_data_plugin_path,
                      GetOrCreateIdaProUserPluginsDirectory());
  NA_RETURN_IF_ERROR(CreateDirectories(idapro_app_data_plugin_path));

  plugin_basename =
      absl::StrFormat("bindiff%s_ida%s", kBinDiffRelease, kLibrarySuffix);
  NA_RETURN_IF_ERROR(CreateOrUpdateLinkWithFallback(
      JoinPath(bindiff_dir, kBinDiffIdaProPluginsPrefix, plugin_basename),
      JoinPath(idapro_app_data_plugin_path, plugin_basename)));

  plugin_basename = absl::StrFormat("binexport%s_ida%s",
                                    kBinDiffBinExportRelease, kLibrarySuffix);
  NA_RETURN_IF_ERROR(CreateOrUpdateLinkWithFallback(
      JoinPath(bindiff_dir, kBinDiffIdaProPluginsPrefix, plugin_basename),
      JoinPath(idapro_app_data_plugin_path, plugin_basename)));

  return absl::OkStatus();
}

using StringSettingsMap = absl::flat_hash_map<std::string, std::string*>;

absl::Status PrintSettingsNames(const StringSettingsMap& settings) {
  std::vector<std::string> names;
  names.reserve(settings.size());
  for (const auto& [key, unused] : settings) {
    names.push_back(key);
  }
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    absl::PrintF("  %s\n", name);
  }
  return absl::OkStatus();
}

absl::Status ApplySettings(const std::vector<char*>& args,
                           const StringSettingsMap& settings) {
  for (const char* arg : args) {
    const std::pair<absl::string_view, absl::string_view> kv =
        absl::StrSplit(arg, absl::MaxSplits('=', 1));
    auto found = settings.find(kv.first);
    if (found == settings.end()) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid config setting: ", kv.first));
    }
    *found->second = kv.second;
  }
  return absl::OkStatus();
}

// Install Abseil Flags' library usage callbacks. This needs to be done before
// any operation that may call one of the callbacks.
void InstallFlagsUsageConfig() {
  absl::FlagsUsageConfig usage_config;
  usage_config.contains_help_flags = [](absl::string_view filename) {
    return !absl::StartsWith(filename, "core library");
  };
  usage_config.contains_helpshort_flags = usage_config.contains_help_flags;
  usage_config.version_string = []() {
    return absl::StrCat(kBinDiffName, " ", kBinDiffDetailedVersion, "\n");
  };
  usage_config.normalize_filename =
      [](absl::string_view filename) -> std::string {
    return absl::StartsWith(filename, "absl") ? "core library" : "this binary";
  };
  absl::SetFlagsUsageConfig(usage_config);
}

absl::Status ConfigSetupMain(int argc, char* argv[]) {
  const std::string binary_name = Basename(argv[0]);
  absl::SetProgramUsageMessage(
      absl::StrFormat("BinDiff config file servicing utility.\n"
                      "Usage: %1$s --config=FILE [KEY=VALUE]...\n"
                      "  or:  %1$s --per_user\n",
                      binary_name));
  InstallFlagsUsageConfig();
  std::vector<char*> positional = absl::ParseCommandLine(argc, argv);
  positional.erase(positional.begin());

  if (absl::GetFlag(FLAGS_per_user)) {
    if (argc != 2) {
      return absl::InvalidArgumentError("Extra arguments to `--per_user`");
    }
    return PerUserSetup(config::Proto());
  }

  // `print_only` loads the config file just like BinDiff itself does.
  auto config =
      !absl::GetFlag(FLAGS_print_only) ? config::Defaults() : config::Proto();

  const StringSettingsMap string_settings = {
      {"directory", config.mutable_directory()},
      {"ida.directory", config.mutable_ida()->mutable_directory()},
      {"log.directory", config.mutable_log()->mutable_directory()},
      {"preferences.default_workspace",
       config.mutable_preferences()->mutable_default_workspace()},
      {"ui.java_binary", config.mutable_ui()->mutable_java_binary()},
      {"ui.server", config.mutable_ui()->mutable_server()},
  };

  if (absl::GetFlag(FLAGS_help_settings)) {
    absl::PrintF("Available settings:\n");
    return PrintSettingsNames(string_settings);
  }

  const std::string config_filename = absl::GetFlag(FLAGS_config);
  if (config_filename.empty()) {
    if (!absl::GetFlag(FLAGS_print_only)) {
      return absl::InvalidArgumentError(
          "Missing config file argument, specify `--config`");
    }
  } else {
    NA_ASSIGN_OR_RETURN(auto loaded_config,
                        config::LoadFromFile(config_filename));
    config::MergeInto(loaded_config, config);
  }

  NA_RETURN_IF_ERROR(ApplySettings(positional, string_settings));

  const std::string serialized = config::AsJsonString(config);
  if (serialized.empty()) {
    return absl::InternalError("Serialization error");
  }

  // Print final config to stdout if requested
  if (absl::GetFlag(FLAGS_print_only)) {
    absl::PrintF("%s", serialized.c_str());
    return absl::OkStatus();
  }

  std::ofstream stream(config_filename,
                       std::ios::out | std::ios::trunc | std::ios::binary);
  stream.write(&serialized[0], serialized.size());
  stream.close();
  if (!stream) {
    return absl::UnknownError(
        absl::StrCat("I/O error writing file: ", GetLastOsError()));
  }
  return absl::OkStatus();
}

}  // namespace security::bindiff

int main(int argc, char** argv) {
  if (auto status = security::bindiff::ConfigSetupMain(argc, argv);
      !status.ok()) {
    absl::FPrintF(stderr, "Error: %s\n", status.message());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
