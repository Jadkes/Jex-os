#pragma once
#include "Window.hpp"

class StatusWindow : public Window {
public:
    StatusWindow(int x, int y, int width, int height, const std::string& title);
    
    void draw() const override;
};
