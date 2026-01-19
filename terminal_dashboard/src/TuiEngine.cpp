#include "TuiEngine.hpp"
#include <iostream>
#include <unistd.h>
#include <thread>
#include <chrono>

TuiEngine::TuiEngine() {
    enableRawMode();
}

TuiEngine::~TuiEngine() {
    disableRawMode();
}

void TuiEngine::addWindow(std::shared_ptr<Window> window) {
    if (windows.empty()) {
        window->setFocused(true);
    }
    windows.push_back(window);
}

void TuiEngine::enableRawMode() {
    tcgetattr(STDIN_FILENO, &original_termios);
    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; // 100ms timeout
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    
    // Hide cursor
    std::cout << "\033[?25l"; 
}

void TuiEngine::disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    // Show cursor and clear screen on exit
    std::cout << "\033[?25h"; 
    std::cout << "\033[2J\033[H";
}

void TuiEngine::clearScreen() {
    std::cout << "\033[2J\033[H";
}

void TuiEngine::render() {
    clearScreen();
    for (const auto& win : windows) {
        win->draw();
    }
    std::cout << std::flush;
}

void TuiEngine::handleInput() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == 'q') {
            running = false;
        } else if (c == '\t') { // Tab to switch windows
            if (!windows.empty()) {
                windows[activeWindowIndex]->setFocused(false);
                activeWindowIndex = (activeWindowIndex + 1) % windows.size();
                windows[activeWindowIndex]->setFocused(true);
            }
        } else {
             if (!windows.empty()) {
                windows[activeWindowIndex]->handleInput(c);
             }
        }
    }
}

void TuiEngine::run() {
    while (running) {
        render();
        handleInput();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
