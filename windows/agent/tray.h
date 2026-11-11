#pragma once

#include <windows.h>

#include "engine.h"

namespace icam::agent {

Settings LoadSettings();
void SaveSettings(const Settings& s);

class Tray {
public:
    explicit Tray(Engine& engine);
    ~Tray();
    bool Create(HINSTANCE inst);

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    void AddIcon();
    void Refresh();
    void ShowMenu();

    Engine& engine_;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    shm::AgentState icon_state_ = shm::AgentState::Unknown;
    UINT taskbar_created_ = 0;
};

}  // namespace icam::agent
