#ifndef OPENMW_APPS_PAPYRUSTOOL_ERRORLOG_H
#define OPENMW_APPS_PAPYRUSTOOL_ERRORLOG_H

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace Papyrus
{
    class ErrorLog
    {
        std::ofstream mOutput;

    public:
        std::function<void(std::string_view, std::string_view)> observe;

        void open(const std::filesystem::path& path)
        {
            if (path.extension() != ".log")
                throw std::runtime_error("Papyrus error log requires a .log file");
            const auto status = std::filesystem::symlink_status(path);
            if (std::filesystem::exists(status)
                && (!std::filesystem::is_regular_file(status) || std::filesystem::hard_link_count(path) != 1))
                throw std::runtime_error("Papyrus error log must be a regular file without links");
            mOutput.open(path, std::ios::app | std::ios::binary);
            if (!mOutput)
                throw std::runtime_error("Cannot open Papyrus error log");
        }

        void write(std::string_view context, std::string_view message) noexcept
        {
            if (observe)
                try
                {
                    observe(context, message);
                }
                catch (...)
                {
                    std::cerr << "Failed forwarding Papyrus error\n";
                }
            if (!mOutput.is_open())
                return;
            const auto writeEscaped = [&](std::string_view text) {
                for (const char c : text)
                {
                    switch (c)
                    {
                        case '\r':
                            mOutput << "\\r";
                            break;
                        case '\n':
                            mOutput << "\\n";
                            break;
                        case '\t':
                            mOutput << "\\t";
                            break;
                        case '\\':
                            mOutput << "\\\\";
                            break;
                        case '"':
                            mOutput << "\\\"";
                            break;
                        default:
                            mOutput << c;
                            break;
                    }
                }
            };
            const auto time = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
            mOutput << "ERROR epoch_ms=" << time.time_since_epoch().count() << " context=\"";
            writeEscaped(context);
            mOutput << "\" message=\"";
            writeEscaped(message);
            mOutput << "\"\n" << std::flush;
            if (!mOutput)
                std::cerr << "Failed writing Papyrus error log\n";
        }
    };
}

#endif
