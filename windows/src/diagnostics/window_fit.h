#pragma once
#include "../common.h"

// 优先原生像素显示；同一分辨率只调整一次，之后保留用户手动改变的窗口尺寸。
void FitStreamWindow(HWND window, int width, int height);
