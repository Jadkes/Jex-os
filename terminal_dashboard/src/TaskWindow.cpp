#include "TaskWindow.hpp"
#include <iostream>

TaskWindow::TaskWindow(int x, int y, int width, int height, const std::string& title)
    : Window(x, y, width, height, title) {
    // Add some sample tasks
    addTask("Buy groceries");
    addTask("Finish C++ project");
    addTask("Call mom");
}

void TaskWindow::addTask(const std::string& desc) {
    tasks.push_back({desc, false});
}

void TaskWindow::toggleTask() {
    if (tasks.empty()) return;
    tasks[selectedIndex].completed = !tasks[selectedIndex].completed;
}

void TaskWindow::deleteTask() {
    if (tasks.empty()) return;
    tasks.erase(tasks.begin() + selectedIndex);
    if (selectedIndex >= tasks.size() && selectedIndex > 0) {
        selectedIndex--;
    }
}

void TaskWindow::draw() const {
    Window::drawBorder();
    
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (i >= height - 2) break; // Don't overflow window

        moveCursor(2, i + 1);
        
        // Draw selection cursor
        if (i == selectedIndex && focused) {
            std::cout << "> ";
        } else {
            std::cout << "  ";
        }

        // Draw checkbox
        std::cout << "[" << (tasks[i].completed ? "x" : " ") << "] ";
        
        // Draw description
        std::cout << tasks[i].description;
    }

    moveCursor(2, height - 2);
    std::cout << "(j/k: move, space: toggle, x: del, a: add)";
}

void TaskWindow::handleInput(char input) {
    switch (input) {
        case 'j':
        case 's':
            if (selectedIndex < tasks.size() - 1) selectedIndex++;
            break;
        case 'k':
        case 'w':
            if (selectedIndex > 0) selectedIndex--;
            break;
        case ' ':
        case '\n':
            toggleTask();
            break;
        case 'x':
            deleteTask();
            break;
        case 'a':
            addTask("New Task " + std::to_string(tasks.size() + 1));
            break;
    }
}
