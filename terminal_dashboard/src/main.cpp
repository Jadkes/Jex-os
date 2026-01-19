#include <iostream>
#include <thread>
#include <chrono>
#include "TuiEngine.hpp"
#include "TaskWindow.hpp"
#include "StatusWindow.hpp"

int main() {
    try {
        TuiEngine engine;
        
        // Setup main layout
        auto taskWin = std::make_shared<TaskWindow>(1, 1, 40, 20, "Tasks");
        auto statusWin = std::make_shared<StatusWindow>(42, 1, 30, 10, "System Status");
        
        engine.addWindow(taskWin);
        engine.addWindow(statusWin); // Corrected variable name from statsWin to statusWin

        engine.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
