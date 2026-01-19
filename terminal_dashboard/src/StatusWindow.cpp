#include "StatusWindow.hpp"
#include <iostream>
#include <ctime>
#include <string>

StatusWindow::StatusWindow(int x, int y, int width, int height, const std::string& title)
    : Window(x, y, width, height, title) {}

void StatusWindow::draw() const {
    Window::drawBorder();

    // Draw Time
    std::time_t now = std::time(nullptr);
    std::string timeStr = std::ctime(&now);
    if (!timeStr.empty()) timeStr.pop_back(); // Remove newline

    moveCursor(2, 2);
    std::cout << "Time: " << timeStr; // Just show basic time for now

    // Pseudo-random stats for demo
    moveCursor(2, 4);
    std::cout << "CPU: " << (rand() % 20 + 10) << "%"; // Fake it till you make it
    
    moveCursor(2, 5);
    std::cout << "RAM: " << (rand() % 40 + 20) << "MB / 8GB";
}
