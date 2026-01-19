#include "Window.hpp"
#include <iostream>

Window::Window(int x, int y, int width, int height, const std::string& title)
    : x(x), y(y), width(width), height(height), title(title) {}

void Window::moveCursor(int localX, int localY) const {
    std::cout << "\033[" << (y + localY) << ";" << (x + localX) << "H";
}

void Window::drawBorder() const {
    // Draw top border
    moveCursor(0, 0);
    std::cout << "+";
    for (int i = 0; i < width - 2; ++i) std::cout << "-";
    std::cout << "+";

    // Draw title if focused
    if (!title.empty()) {
        moveCursor(2, 0);
        if (focused) std::cout << "\033[7m " << title << " \033[0m"; // Inverse video for focus
        else std::cout << " " << title << " ";
    }

    // Draw side borders
    for (int i = 1; i < height - 1; ++i) {
        moveCursor(0, i);
        std::cout << "|";
        moveCursor(width - 1, i);
        std::cout << "|";
    }

    // Draw bottom border
    moveCursor(0, height - 1);
    std::cout << "+";
    for (int i = 0; i < width - 2; ++i) std::cout << "-";
    std::cout << "+";
}

void Window::draw() const {
    drawBorder();
}

void Window::handleInput(char input) {
    // Base window does nothing with input
}
