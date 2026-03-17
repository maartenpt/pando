#include "corpus/builder.h"
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

static std::vector<std::string> find_conllu_files(const std::string& path) {
    std::vector<std::string> files;

    if (fs::is_regular_file(path)) {
        files.push_back(path);
    } else if (fs::is_directory(path)) {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_regular_file() &&
                entry.path().extension() == ".conllu")
                files.push_back(entry.path().string());
        }
        std::sort(files.begin(), files.end());
    } else {
        throw std::runtime_error("Not a file or directory: " + path);
    }

    if (files.empty())
        throw std::runtime_error("No .conllu files found in " + path);

    return files;
}

static std::vector<std::string> find_vertical_files(const std::string& path) {
    std::vector<std::string> files;
    if (fs::is_regular_file(path)) {
        files.push_back(path);
    } else if (fs::is_directory(path)) {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_regular_file()) {
                auto ext = entry.path().extension().string();
                if (ext == ".vrt" || ext == ".vert" || ext == ".txt")
                    files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
    } else {
        throw std::runtime_error("Not a file or directory: " + path);
    }
    if (files.empty())
        throw std::runtime_error("No .vrt/.vert/.txt files found in " + path);
    return files;
}

int main(int argc, char* argv[]) {
    bool split_feats = false;
    bool format_vertical = false;
    bool format_jsonl = false;

    // Collect flags
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--split-feats") split_feats = true;
        else if (a == "--format" && i + 1 < argc) {
            std::string fmt = argv[++i];
            if (fmt == "vertical") format_vertical = true;
            else if (fmt == "jsonl") format_jsonl = true;
        } else if (a == "--format") {
            /* missing arg */
        } else {
            args.push_back(a);
        }
    }

    if (args.size() < 2) {
        std::cerr << "Usage: pando-index [options] <input> <output_dir>\n\n"
                  << "  input: .conllu file(s), directory (recursive), or '-' for JSONL stdin;\n"
                  << "         with --format vertical: .vrt/.vert/.txt files;\n"
                  << "         with --format jsonl: JSONL events as in dev/PANDO-INDEX-INTEGRATION.md\n\n"
                  << "  --split-feats     Split FEATS into feats_X (default: combined)\n"
                  << "  --format vertical Read CWB-style vertical (one token/line, <s> </s>)\n"
                  << "  --format jsonl    Read streaming JSONL events (tokens/regions)\n";
        return 1;
    }

    std::string input_path = args[0];
    std::string output_dir = args[1];

    try {
        manatree::CorpusBuilder builder(output_dir);
        builder.set_split_feats(split_feats);

        auto t0 = std::chrono::steady_clock::now();
        int64_t prev_tokens = 0;
        auto prev_time = t0;
        if (format_jsonl) {
            // JSONL: single stream from file or stdin ("-").
            std::cerr << "Reading JSONL from " << input_path << "\n";
            builder.read_jsonl(input_path);
        } else {
            std::vector<std::string> files;

            if (format_vertical) {
                files = find_vertical_files(input_path);
                std::cerr << "Found " << files.size() << " vertical file"
                          << (files.size() != 1 ? "s" : "") << "\n";
            } else {
                files = find_conllu_files(input_path);
                std::cerr << "Found " << files.size() << " .conllu file"
                          << (files.size() != 1 ? "s" : "") << "\n";
            }

            for (size_t i = 0; i < files.size(); ++i) {
                std::cerr << "[" << (i + 1) << "/" << files.size() << "] "
                          << files[i];

                if (format_vertical)
                    builder.read_vertical(files[i]);
                else
                    builder.read_conllu(files[i]);

                int64_t cur_tokens = builder.builder().corpus_size();
                auto now = std::chrono::steady_clock::now();
                double secs = std::chrono::duration<double>(now - prev_time).count();
                if (secs > 0.001) {
                    double ktps = static_cast<double>(cur_tokens - prev_tokens) / secs / 1000.0;
                    std::cerr << "  (" << (cur_tokens - prev_tokens) << " tok, "
                              << static_cast<int>(ktps) << " ktok/s)";
                }
                std::cerr << "\n";
                prev_tokens = cur_tokens;
                prev_time = now;
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        double total_secs = std::chrono::duration<double>(t1 - t0).count();
        int64_t total_tokens = builder.builder().corpus_size();
        std::cerr << "Corpus: " << total_tokens << " tokens in "
                  << static_cast<int>(total_secs) << "s ("
                  << static_cast<int>(total_tokens / total_secs / 1000) << " ktok/s avg)\n";
        std::cerr << "Finalizing...\n";
        builder.finalize();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
