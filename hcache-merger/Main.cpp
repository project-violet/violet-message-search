//===----------------------------------------------------------------------===//
//
//                             HCache-Merger
//
//===----------------------------------------------------------------------===//
//
//  Copyright (C) 2021. violet-dev. All Rights Reserved.
//
//===----------------------------------------------------------------------===//

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "json.hpp"

const std::string target_dir = "/home/ubuntu/htext-miner/cache";

int main() {
  nlohmann::json result;
  int count {};
   for (auto& p : std::filesystem::directory_iterator(target_dir))
      ++count;
  int count_process = 0;
  for (const auto &entry : std::filesystem::directory_iterator(target_dir)) {
    std::cout << ++count_process << '/' << count << entry.path() << std::endl;

    std::ifstream i(entry.path());
    nlohmann::json j;
    i >> j;

    for (const auto &item : j.items())
        result.push_back(item.value());
  }
  std::ofstream o("merged.json");
  o << result << std::endl;
}