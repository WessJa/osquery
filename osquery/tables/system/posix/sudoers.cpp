/**
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under both the Apache 2.0 license (found in the
 *  LICENSE file in the root directory of this source tree) and the GPLv2 (found
 *  in the COPYING file in the root directory of this source tree).
 *  You may select, at your option, one of the above-listed licenses.
 */

#include <locale>
#include <vector>

#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/path.hpp>

#include <osquery/filesystem/filesystem.h>
#include <osquery/logger.h>
#include <osquery/tables.h>
#include <osquery/utils/conversions/join.h>
#include <osquery/utils/conversions/split.h>

namespace fs = boost::filesystem;

namespace osquery {
namespace tables {

#if !defined(FREEBSD)
const std::string kSudoFile = "/etc/sudoers";
#else
const std::string kSudoFile = "/usr/local/etc/sudoers";
#endif

// sudoers(5): No more than 128 files are allowed to be nested.
static const unsigned int kMaxNest = 128;

void genSudoersFile(const std::string& filename,
                    unsigned int level,
                    QueryData& results) {
  if (level > kMaxNest) {
    TLOG << "sudoers file recursion maximum reached";
    return;
  }

  std::string contents;
  if (!forensicReadFile(filename, contents).ok()) {
    TLOG << "couldn't read sudoers file: " << filename;
    return;
  }

  auto lines = split(contents, "\n");
  for (auto& line : lines) {
    Row r;
    boost::trim(line);

    // Only add lines that are not comments or blank.
    if (line.size() > 0 && line.at(0) != '#') {
      r["source"] = filename;

      auto header_pos = line.find_first_of("\t\v ");
      r["header"] = line.substr(0, header_pos);

      if (header_pos == std::string::npos) {
        header_pos = line.size() - 1;
      }

      r["rule_details"] = line.substr(header_pos + 1);

      results.push_back(std::move(r));
    } else if (line.find("#includedir") == 0) {
      auto space = line.find_first_of(' ');

      // If #includedir doesn't look like it's followed by
      // a path, treat it like a normal comment.
      if (space == std::string::npos) {
        continue;
      }

      auto inc_dir = line.substr(space + 1);

      // NOTE(ww): See sudo NEWS for 1.8.4:
      // Both #include and #includedir support relative paths.
      if (inc_dir.at(0) != '/') {
        auto path = fs::path(filename).parent_path() / inc_dir;
        inc_dir = path.string();
      }

      // Build and push the row before recursing.
      r["source"] = filename;
      r["header"] = "#includedir";
      r["rule_details"] = inc_dir;
      results.push_back(std::move(r));

      std::vector<std::string> inc_files;
      if (!listFilesInDirectory(inc_dir, inc_files).ok()) {
        TLOG << "couldn't list includedir: " << inc_dir;
        continue;
      }

      for (const auto& inc_file : inc_files) {
        std::string inc_basename = fs::path(inc_file).filename().string();

        // Per sudoers(5): Any files in the included directory that
        // contain a '.' or end with '~' are ignored.
        if (inc_basename.empty() ||
            inc_basename.find('.') != std::string::npos ||
            inc_basename.back() == '~') {
          continue;
        }

        genSudoersFile(inc_file, ++level, results);
      }
    } else if (line.find("#include") == 0) {
      auto space = line.find_first_of(' ');

      // If #include doesn't look like it's followed by
      // a path, treat it like a normal comment.
      if (space == std::string::npos) {
        continue;
      }

      auto inc_file = line.substr(space + 1);

      // Per sudoers(5): If the included file doesn't
      // start with /, read it relative to the current file.
      if (inc_file.at(0) != '/') {
        const auto path = fs::path(filename).parent_path() / inc_file;
        inc_file = path.string();
      }

      r["source"] = filename;
      r["header"] = "#include";
      r["rule_details"] = inc_file;
      results.push_back(std::move(r));

      genSudoersFile(inc_file, ++level, results);
    }
  }
}

QueryData genSudoers(QueryContext& context) {
  QueryData results;

  if (!isReadable(kSudoFile).ok()) {
    return results;
  }

  genSudoersFile(kSudoFile, 1, results);

  return results;
}
} // namespace tables
} // namespace osquery
