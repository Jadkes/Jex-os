#pragma once
#include "Window.hpp"
#include <vector>
#include <string>

struct Task {
    std::string description;
    bool completed;
};

class TaskWindow : public Window {
public:
    TaskWindow(int x, int y, int width, int height, const std::string& title);
    
    void draw() const override;
    void handleInput(char input) override;

private:
    std::vector<Task> tasks;
    int selectedIndex = 0;

    void addTask(const std::string& desc);
    void toggleTask();
    void deleteTask();
};
