#pragma once
#include "../src/common.h"

// GPU 桌面复制被拒时记录会话、桌面、前台程序、同名进程和注入模块，用于区分远程会话、残留进程与防截屏软件。
void LogDuplicationDenied();
