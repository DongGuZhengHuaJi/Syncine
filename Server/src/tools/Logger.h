//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_LOGGER_H
#define SERVER_LOGGER_H

#include <string>
#include <iostream>

class Logger {
public:
    static void info(const std::string& message) {
        std::cout << "[INFO] " << message << std::endl;
    }

    static void warning(const std::string& message) {
        std::cout << "[WARNING] " << message << std::endl;
    }

    static void error(const std::string& message) {
        std::cerr << "[ERROR] " << message << std::endl;
    }
};


#endif //SERVER_LOGGER_H
