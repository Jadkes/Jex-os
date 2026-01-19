#pragma once
#include <vector>
#include <memory>
#include <termios.h>
#include "Window.hpp"

class TuiEngine {
public:
    TuiEngine();
    ~TuiEngine();

    void addWindow(std::shared_ptr<Window> window);
    void run();

private:
    struct termios original_termios;
    std::vector<std::shared_ptr<Window>> windows;
    bool running = true;
    int activeWindowIndex = 0;

    void enableRawMode();
    void disableRawMode();
    void clearScreen();
    void render();
    void handleInput();
};
