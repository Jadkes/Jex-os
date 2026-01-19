#pragma once
#include <string>
#include <vector>
#include <memory>

class Window {
public:
    Window(int x, int y, int width, int height, const std::string& title);
    virtual ~Window() = default;

    virtual void draw() const;
    virtual void handleInput(char input);

    // Getters and Setters
    int getX() const { return x; }
    int getY() const { return y; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    bool isFocused() const { return focused; }
    void setFocused(bool f) { focused = f; }

protected:
    int x, y;
    int width, height;
    std::string title;
    bool focused = false;

    // Helper to move cursor relative to window
    void moveCursor(int localX, int localY) const;
    void drawBorder() const;
};
