#pragma once

#include <filesystem>

int run_journal_demo(const std::filesystem::path& path = {});
int run_replay(const std::filesystem::path& path);
