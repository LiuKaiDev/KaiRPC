#ifndef KAIRPC_CONFIG_READER_H
#define KAIRPC_CONFIG_READER_H

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

class ConfigReader {
public:
    explicit ConfigReader(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            parseLine(line);
        }
    }

    std::unordered_map<std::string, std::string> getMap() const {
        return configMap;
    }

private:
    std::unordered_map<std::string, std::string> configMap;

    void parseLine(const std::string& line) {
        std::string trimmedLine = trim(line);
        if (trimmedLine.empty() || trimmedLine[0] == '#') {
            return;
        }

        std::size_t pos = trimmedLine.find('=');
        if (pos == std::string::npos) {
            return;
        }

        std::string key = trim(trimmedLine.substr(0, pos));
        std::string value = trim(trimmedLine.substr(pos + 1));
        configMap[key] = value;
    }

    static std::string trim(const std::string& str) {
        std::size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }

        std::size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
    }
};

#endif
