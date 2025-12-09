// snake_smooth_mt_optimized.cpp
// Smooth-interpolated Snake + multithreaded renderer (Win32 GDI) - OPTIMIZED
// Compile: g++ snake_smooth_mt_optimized.cpp -std=c++17 -lgdi32 -o snake.exe

#include <windows.h>
#include <vector>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>

//
// Config
//
static int CELL = 80;
static int GRID_W = 10;
static int GRID_H = 10;
static constexpr int WIN_EXTRA_H = 40;
static int TICK_INTERVAL_MS_VALUE = 120; // medium speed
static int TARGET_FPS = 240;

//
// Settings options
//
static int settingSelection = 0; // which setting is selected
static int fpsOptions[] = { 60, 120, 180, 240 };
static int fpsIndex = 3; // default 240
static int cellSize = 80; // 60-120
static int gridWidth = 10; // 5-40
static int gridHeight = 10; // 5-40
static int fruitCount = 1; // 1-15
static int speedOptions[] = { 180, 120, 80 }; // easy, medium, hard
static const wchar_t* speedNames[] = { L"Easy", L"Medium", L"Hard" };
static int speedIndex = 1; // default medium (was 0!)

//
// Types
//
struct Pt { int x, y; };
struct FPt { float x, y; };

enum Direction { UP, DOWN, LEFT, RIGHT };

enum GameState { MENU, SETTINGS, PLAYING };

//
// Shared game state
//
static GameState gameState = MENU;
static int menuSelection = 0; // 0=Play, 1=Settings, 2=Exit
static int pauseSelection = 0; // 0=Resume, 1=Menu
static int gameOverSelection = 0; // 0=Restart, 1=Menu
static std::deque<Pt> currSnake;
static std::deque<Pt> prevSnake;
static std::vector<Pt> food; // multiple food items
static Direction dir = RIGHT;
static Direction nextDir = RIGHT; // FIXED: queue next direction
static bool gameOver = false;
static bool gameWon = false;
static bool paused = false;
static bool started = false; // NEW: game hasn't started yet
static int score = 0;

static std::chrono::steady_clock::time_point lastTickTime = std::chrono::steady_clock::now();
static std::chrono::milliseconds tickDuration = std::chrono::milliseconds(TICK_INTERVAL_MS_VALUE);

static std::mutex stateMtx;
static std::mutex rngMtx; // FIXED: separate mutex for RNG
static std::atomic_bool running{ true };

// RNG - now protected by rngMtx
static std::mt19937 rng((unsigned)std::random_device{}());
static std::uniform_int_distribution<int> distW(0, GRID_W - 1);
static std::uniform_int_distribution<int> distH(0, GRID_H - 1);

static HWND g_hwnd = nullptr;

//
// Resize window to match current grid settings
//
static void resizeWindow() {
    if (g_hwnd) {
        int winW = GRID_W * CELL;
        int winH = GRID_H * CELL + WIN_EXTRA_H;

        // Get window rect to account for borders
        RECT windowRect = { 0, 0, winW, winH };
        AdjustWindowRect(&windowRect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

        int actualWidth = windowRect.right - windowRect.left;
        int actualHeight = windowRect.bottom - windowRect.top;

        SetWindowPos(g_hwnd, NULL, 0, 0, actualWidth, actualHeight,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

//
// Helpers
//
static Pt moveHead(const Pt& h, Direction d) {
    Pt n = h;
    switch (d) {
    case UP:    n.y -= 1; break;
    case DOWN:  n.y += 1; break;
    case LEFT:  n.x -= 1; break;
    case RIGHT: n.x += 1; break;
    }
    return n;
}

static void placeFoodLocked() {
    // caller holds stateMtx, but we need rngMtx for RNG
    std::lock_guard<std::mutex> lk(rngMtx);

    food.clear();
    int numFood = min(fruitCount, GRID_W * GRID_H - (int)currSnake.size());

    for (int i = 0; i < numFood; i++) {
        int attempts = 0;
        while (attempts < 1000) {
            Pt p{ distW(rng), distH(rng) };
            bool valid = true;

            // Check if on snake
            for (auto& s : currSnake) {
                if (s.x == p.x && s.y == p.y) {
                    valid = false;
                    break;
                }
            }

            // Check if on existing food
            if (valid) {
                for (auto& f : food) {
                    if (f.x == p.x && f.y == p.y) {
                        valid = false;
                        break;
                    }
                }
            }

            if (valid) {
                food.push_back(p);
                break;
            }
            attempts++;
        }
    }
}

static void placeOneFoodLocked() {
    // Place a single new fruit (when one is eaten)
    std::lock_guard<std::mutex> lk(rngMtx);

    int attempts = 0;
    while (attempts < 1000) {
        Pt p{ distW(rng), distH(rng) };
        bool valid = true;

        // Check if on snake
        for (auto& s : currSnake) {
            if (s.x == p.x && s.y == p.y) {
                valid = false;
                break;
            }
        }

        // Check if on existing food
        if (valid) {
            for (auto& f : food) {
                if (f.x == p.x && f.y == p.y) {
                    valid = false;
                    break;
                }
            }
        }

        if (valid) {
            food.push_back(p);
            break;
        }
        attempts++;
    }
}

static void resetGameLocked() {
    // Apply settings
    CELL = cellSize;
    GRID_W = gridWidth;
    GRID_H = gridHeight;
    TICK_INTERVAL_MS_VALUE = speedOptions[speedIndex];
    TARGET_FPS = fpsOptions[fpsIndex];

    // Resize window to match new grid size
    resizeWindow();

    // Update RNG distributions
    distW = std::uniform_int_distribution<int>(0, GRID_W - 1);
    distH = std::uniform_int_distribution<int>(0, GRID_H - 1);

    currSnake.clear();
    int sx = GRID_W / 2;
    int sy = GRID_H / 2;
    currSnake.push_back({ sx, sy });
    currSnake.push_back({ sx - 1, sy });
    currSnake.push_back({ sx - 2, sy });
    prevSnake = currSnake; // Keep in sync for rendering
    dir = RIGHT;
    nextDir = RIGHT;
    gameOver = false;
    gameWon = false;
    paused = false;
    started = false;
    score = 0;
    placeFoodLocked();
    lastTickTime = std::chrono::steady_clock::now();
    tickDuration = std::chrono::milliseconds(TICK_INTERVAL_MS_VALUE);
}

//
// Game tick
//
static void gameThreadFunc() {
    using clock = std::chrono::steady_clock;
    auto nextTick = clock::now() + std::chrono::milliseconds(TICK_INTERVAL_MS_VALUE);

    while (running) {
        auto now = clock::now();

        // Check if we should tick
        bool shouldTick = false;
        {
            std::lock_guard<std::mutex> lk(stateMtx);
            shouldTick = started && !paused && !gameOver && !gameWon;

            // If not active, reset the next tick time to prevent accumulated time
            if (!shouldTick) {
                nextTick = now + std::chrono::milliseconds(TICK_INTERVAL_MS_VALUE);
                prevSnake = currSnake; // Keep in sync for rendering
            }
        }

        // Perform game tick if it's time
        if (shouldTick && now >= nextTick) {
            nextTick += std::chrono::milliseconds(TICK_INTERVAL_MS_VALUE);

            std::lock_guard<std::mutex> lk(stateMtx);
            if (started && !paused && !gameOver && !gameWon) {
                // Apply queued direction at start of tick
                dir = nextDir;

                Pt head = currSnake.front();
                Pt newHead = moveHead(head, dir);

                // collision check
                bool collided = false;
                if (newHead.x < 0 || newHead.x >= GRID_W || newHead.y < 0 || newHead.y >= GRID_H) {
                    collided = true;
                }
                else {
                    for (auto& s : currSnake) {
                        if (s.x == newHead.x && s.y == newHead.y) {
                            collided = true;
                            break;
                        }
                    }
                }

                // snapshot prevSnake before modifying currSnake
                prevSnake = currSnake;

                if (collided) {
                    gameOver = true;
                }
                else {
                    currSnake.push_front(newHead);

                    // Check if ate any food
                    bool ateFood = false;
                    for (auto it = food.begin(); it != food.end(); ++it) {
                        if (newHead.x == it->x && newHead.y == it->y) {
                            score += 10;
                            food.erase(it);
                            ateFood = true;
                            break;
                        }
                    }

                    if (ateFood) {
                        // Check if won (snake fills entire grid)
                        if (currSnake.size() >= (size_t)(GRID_W * GRID_H)) {
                            gameWon = true;
                        }
                        else {
                            // Place only ONE new fruit to replace the eaten one
                            placeOneFoodLocked();
                        }
                    }
                    else {
                        currSnake.pop_back();
                    }
                }

                lastTickTime = clock::now();
                tickDuration = std::chrono::milliseconds(TICK_INTERVAL_MS_VALUE);
            }
        }

        // Short sleep to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

//
// Interpolation
//
static FPt lerp(const FPt& a, const FPt& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

//
// Render snapshot
//
struct RenderSnapshot {
    std::vector<FPt> prev;
    std::vector<FPt> curr;
    std::vector<FPt> food;
    int score;
    bool gameOver;
    bool gameWon;
    bool paused;
    bool started;
    GameState state;
    int menuSelection;
    int pauseSelection;
    int gameOverSelection;
    int settingSelection;
    int fpsIndex;
    int cellSize;
    int gridWidth;
    int gridHeight;
    int fruitCount;
    int speedIndex;
    std::chrono::steady_clock::time_point tickTime;
    std::chrono::milliseconds tickDur;
};

//
// GDI Resource cache to avoid recreating every frame
//
struct GDICache {
    HBRUSH bgBrush;
    HBRUSH foodBrush;
    HBRUSH headBrush;
    HBRUSH bodyBrush1;
    HBRUSH bodyBrush2;
    HBRUSH overBrush;
    HPEN gridPen;
    HPEN headPen;
    HPEN bodyPen;
    HFONT scoreFont;
    HFONT gameOverFont;
    HFONT menuTitleFont;
    HFONT menuButtonFont;

    GDICache() {
        bgBrush = CreateSolidBrush(RGB(22, 26, 30));
        foodBrush = CreateSolidBrush(RGB(255, 70, 70));
        headBrush = CreateSolidBrush(RGB(90, 220, 90));
        bodyBrush1 = CreateSolidBrush(RGB(40, 170, 40));
        bodyBrush2 = CreateSolidBrush(RGB(30, 140, 30));
        overBrush = CreateSolidBrush(RGB(0, 0, 0));

        gridPen = CreatePen(PS_SOLID, 1, RGB(40, 40, 48));
        headPen = CreatePen(PS_SOLID, 1, RGB(0, 110, 0));
        bodyPen = CreatePen(PS_SOLID, 1, RGB(0, 90, 0));

        scoreFont = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        gameOverFont = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        menuTitleFont = CreateFontW(64, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        menuButtonFont = CreateFontW(28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }

    ~GDICache() {
        DeleteObject(bgBrush);
        DeleteObject(foodBrush);
        DeleteObject(headBrush);
        DeleteObject(bodyBrush1);
        DeleteObject(bodyBrush2);
        DeleteObject(overBrush);
        DeleteObject(gridPen);
        DeleteObject(headPen);
        DeleteObject(bodyPen);
        DeleteObject(scoreFont);
        DeleteObject(gameOverFont);
        DeleteObject(menuTitleFont);
        DeleteObject(menuButtonFont);
    }
};

//
// Render thread
//
static void renderThreadFunc() {
    using clock = std::chrono::steady_clock;
    RenderSnapshot snap;
    GDICache cache; // OPTIMIZED: reuse GDI objects

    // Pre-reserve vectors to reduce allocations
    snap.prev.reserve(100);
    snap.curr.reserve(100);

    while (running) {
        auto frameStart = clock::now();

        // Copy state under lock
        {
            std::lock_guard<std::mutex> lk(stateMtx);
            snap.prev.clear();
            snap.curr.clear();
            snap.food.clear();

            for (auto& p : prevSnake) snap.prev.push_back({ float(p.x), float(p.y) });
            for (auto& p : currSnake) snap.curr.push_back({ float(p.x), float(p.y) });
            for (auto& f : food) snap.food.push_back({ float(f.x), float(f.y) });

            snap.score = score;
            snap.gameOver = gameOver;
            snap.gameWon = gameWon;
            snap.paused = paused;
            snap.started = started;
            snap.state = gameState;
            snap.menuSelection = menuSelection;
            snap.pauseSelection = pauseSelection;
            snap.gameOverSelection = gameOverSelection;
            snap.settingSelection = settingSelection;
            snap.fpsIndex = fpsIndex;
            snap.cellSize = cellSize;
            snap.gridWidth = gridWidth;
            snap.gridHeight = gridHeight;
            snap.fruitCount = fruitCount;
            snap.speedIndex = speedIndex;
            snap.tickTime = lastTickTime;
            snap.tickDur = tickDuration;
        }

        // Compute interpolation alpha
        float alpha = 1.0f;
        {
            auto now = clock::now();
            auto elapsed = now - snap.tickTime;
            float a = float(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) /
                float(std::chrono::duration_cast<std::chrono::microseconds>(snap.tickDur).count());
            alpha = std::clamp(a, 0.0f, 1.0f); // FIXED: use clamp
        }

        // Render
        HDC hdc = GetDC(g_hwnd);
        if (hdc) {
            RECT client;
            GetClientRect(g_hwnd, &client);
            int winW = client.right - client.left;
            int winH = client.bottom - client.top;

            // Double buffer
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, winW, winH);
            HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);

            // Background
            FillRect(memDC, &client, cache.bgBrush);

            // Render menu if in menu state
            if (snap.state == MENU) {
                // Title - scale font with window
                int titleFontSize = max(32, min(64, (GRID_W * CELL) / 8));
                HFONT titleFont = CreateFontW(titleFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT oldTitle = (HFONT)SelectObject(memDC, titleFont);
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, RGB(90, 220, 90));
                int titleY = max(60, GRID_H * CELL / 6);
                RECT titleRect = { 0, titleY, GRID_W * CELL, titleY + titleFontSize + 20 };
                DrawTextW(memDC, L"SNAKE", -1, &titleRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(memDC, oldTitle);
                DeleteObject(titleFont);

                // Menu buttons - scale font
                int buttonFontSize = max(18, min(28, (GRID_W * CELL) / 16));
                HFONT buttonFont = CreateFontW(buttonFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT oldButton = (HFONT)SelectObject(memDC, buttonFont);

                // Calculate button positions - scale with grid size
                int buttonWidth = max(140, min(220, GRID_W * CELL - 100));
                int buttonHeight = max(35, min(55, GRID_H * CELL / 9));
                int centerX = (GRID_W * CELL) / 2;
                int startY = max(140, (GRID_H * CELL - (3 * buttonHeight + 2 * 65)) / 2);
                int buttonSpacing = max(55, buttonHeight + 15);

                // Play button
                RECT playRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                HBRUSH playBrush = CreateSolidBrush(snap.menuSelection == 0 ? RGB(50, 200, 50) : RGB(40, 170, 40));
                FillRect(memDC, &playRect, playBrush);
                DeleteObject(playBrush);
                HPEN buttonPen = CreatePen(PS_SOLID, snap.menuSelection == 0 ? 3 : 2, RGB(90, 220, 90));
                HPEN oldButtonPen = (HPEN)SelectObject(memDC, buttonPen);
                SelectObject(memDC, GetStockObject(NULL_BRUSH));
                Rectangle(memDC, playRect.left, playRect.top, playRect.right, playRect.bottom);
                SelectObject(memDC, oldButtonPen);
                DeleteObject(buttonPen);
                SetTextColor(memDC, RGB(220, 220, 220));
                DrawTextW(memDC, L"Play", -1, &playRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // Settings button
                startY += buttonSpacing;
                RECT settingsRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                HBRUSH settingsBrush = CreateSolidBrush(snap.menuSelection == 1 ? RGB(50, 200, 50) : RGB(40, 170, 40));
                FillRect(memDC, &settingsRect, settingsBrush);
                DeleteObject(settingsBrush);
                buttonPen = CreatePen(PS_SOLID, snap.menuSelection == 1 ? 3 : 2, RGB(90, 220, 90));
                oldButtonPen = (HPEN)SelectObject(memDC, buttonPen);
                SelectObject(memDC, GetStockObject(NULL_BRUSH));
                Rectangle(memDC, settingsRect.left, settingsRect.top, settingsRect.right, settingsRect.bottom);
                SelectObject(memDC, oldButtonPen);
                DeleteObject(buttonPen);
                SetTextColor(memDC, RGB(220, 220, 220));
                DrawTextW(memDC, L"Settings", -1, &settingsRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // Exit button
                startY += buttonSpacing;
                RECT exitRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                HBRUSH exitBrush = CreateSolidBrush(snap.menuSelection == 2 ? RGB(200, 50, 50) : RGB(170, 40, 40));
                FillRect(memDC, &exitRect, exitBrush);
                DeleteObject(exitBrush);
                buttonPen = CreatePen(PS_SOLID, snap.menuSelection == 2 ? 3 : 2, RGB(220, 90, 90));
                oldButtonPen = (HPEN)SelectObject(memDC, buttonPen);
                SelectObject(memDC, GetStockObject(NULL_BRUSH));
                Rectangle(memDC, exitRect.left, exitRect.top, exitRect.right, exitRect.bottom);
                SelectObject(memDC, oldButtonPen);
                DeleteObject(buttonPen);
                SetTextColor(memDC, RGB(220, 220, 220));
                DrawTextW(memDC, L"Exit", -1, &exitRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(memDC, oldButton);
                DeleteObject(buttonFont);
            }
            // Render settings screen
            else if (snap.state == SETTINGS) {
                // Title - scale font
                int titleFontSize = max(32, min(64, (GRID_W * CELL) / 8));
                HFONT titleFont = CreateFontW(titleFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT oldTitle = (HFONT)SelectObject(memDC, titleFont);
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, RGB(90, 220, 90));
                RECT titleRect = { 0, 40, GRID_W * CELL, 100 };
                DrawTextW(memDC, L"SETTINGS", -1, &titleRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(memDC, oldTitle);
                DeleteObject(titleFont);

                int settingsFontSize = max(16, min(22, (GRID_W * CELL) / 20));
                HFONT settingsFont = CreateFontW(settingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT oldSettings = (HFONT)SelectObject(memDC, settingsFont);

                int leftCol = max(30, GRID_W * CELL / 10);
                int rightCol = max(200, GRID_W * CELL / 2);
                int startY = max(120, GRID_H * CELL / 4);
                int rowHeight = max(35, min(50, GRID_H * CELL / 10));

                // FPS
                SetTextColor(memDC, snap.settingSelection == 0 ? RGB(90, 220, 90) : RGB(180, 180, 180));
                TextOutW(memDC, leftCol, startY, L"FPS:", 4);
                std::wstring fpsVal = std::to_wstring(fpsOptions[snap.fpsIndex]);
                SetTextColor(memDC, snap.settingSelection == 0 ? RGB(220, 220, 220) : RGB(150, 150, 150));
                TextOutW(memDC, rightCol, startY, fpsVal.c_str(), (int)fpsVal.size());

                // Cell Size
                startY += rowHeight;
                SetTextColor(memDC, snap.settingSelection == 1 ? RGB(90, 220, 90) : RGB(180, 180, 180));
                TextOutW(memDC, leftCol, startY, L"Cell Size:", 10);
                std::wstring cellVal = std::to_wstring(snap.cellSize);
                SetTextColor(memDC, snap.settingSelection == 1 ? RGB(220, 220, 220) : RGB(150, 150, 150));
                TextOutW(memDC, rightCol, startY, cellVal.c_str(), (int)cellVal.size());

                // Grid Width
                startY += rowHeight;
                SetTextColor(memDC, snap.settingSelection == 2 ? RGB(90, 220, 90) : RGB(180, 180, 180));
                TextOutW(memDC, leftCol, startY, L"Grid Width:", 11);
                std::wstring widthVal = std::to_wstring(snap.gridWidth);
                SetTextColor(memDC, snap.settingSelection == 2 ? RGB(220, 220, 220) : RGB(150, 150, 150));
                TextOutW(memDC, rightCol, startY, widthVal.c_str(), (int)widthVal.size());

                // Grid Height
                startY += rowHeight;
                SetTextColor(memDC, snap.settingSelection == 3 ? RGB(90, 220, 90) : RGB(180, 180, 180));
                TextOutW(memDC, leftCol, startY, L"Grid Height:", 12);
                std::wstring heightVal = std::to_wstring(snap.gridHeight);
                SetTextColor(memDC, snap.settingSelection == 3 ? RGB(220, 220, 220) : RGB(150, 150, 150));
                TextOutW(memDC, rightCol, startY, heightVal.c_str(), (int)heightVal.size());

                // Game Speed
                startY += rowHeight;
                SetTextColor(memDC, snap.settingSelection == 4 ? RGB(90, 220, 90) : RGB(180, 180, 180));
                TextOutW(memDC, leftCol, startY, L"Speed:", 6);
                SetTextColor(memDC, snap.settingSelection == 4 ? RGB(220, 220, 220) : RGB(150, 150, 150));
                TextOutW(memDC, rightCol, startY, speedNames[snap.speedIndex], (int)wcslen(speedNames[snap.speedIndex]));

                // Fruit Count
                startY += rowHeight;
                SetTextColor(memDC, snap.settingSelection == 5 ? RGB(90, 220, 90) : RGB(180, 180, 180));
                TextOutW(memDC, leftCol, startY, L"Fruit Count:", 12);
                std::wstring fruitVal = std::to_wstring(snap.fruitCount);
                SetTextColor(memDC, snap.settingSelection == 5 ? RGB(220, 220, 220) : RGB(150, 150, 150));
                TextOutW(memDC, rightCol, startY, fruitVal.c_str(), (int)fruitVal.size());

                // Back button
                startY += rowHeight + 20;
                SetTextColor(memDC, snap.settingSelection == 6 ? RGB(220, 90, 90) : RGB(180, 180, 180));
                RECT backRect = { 0, startY, GRID_W * CELL, startY + 30 };
                DrawTextW(memDC, L"< Back to Menu", -1, &backRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(memDC, oldSettings);
                DeleteObject(settingsFont);
            }
            // Render game if playing
            else {
                // Grid lines
                HPEN oldPen = (HPEN)SelectObject(memDC, cache.gridPen);
                for (int x = 0; x <= GRID_W * CELL; x += CELL) {
                    MoveToEx(memDC, x, 0, NULL);
                    LineTo(memDC, x, GRID_H * CELL);
                }
                for (int y = 0; y <= GRID_H * CELL; y += CELL) {
                    MoveToEx(memDC, 0, y, NULL);
                    LineTo(memDC, GRID_W * CELL, y);
                }
                SelectObject(memDC, oldPen);

                // Food - draw all food items
                HBRUSH foodBrush = CreateSolidBrush(RGB(255, 70, 70));
                for (auto& f : snap.food) {
                    RECT fr = {
                        int(f.x * CELL),
                        int(f.y * CELL),
                        int(f.x * CELL + CELL),
                        int(f.y * CELL + CELL)
                    };
                    FillRect(memDC, &fr, foodBrush);
                }
                DeleteObject(foodBrush);

                // Snake with interpolation
                size_t nSegments = snap.curr.size();
                for (size_t i = 0; i < nSegments; ++i) {
                    FPt a = (i < snap.prev.size()) ? snap.prev[i] : snap.curr[i];
                    FPt b = snap.curr[i];
                    FPt ip = lerp(a, b, alpha);

                    RECT sr = {
                        int(ip.x * CELL) + 1,
                        int(ip.y * CELL) + 1,
                        int(ip.x * CELL + CELL) - 1,
                        int(ip.y * CELL + CELL) - 1
                    };

                    if (i == 0) {
                        // Head
                        FillRect(memDC, &sr, cache.headBrush);
                        oldPen = (HPEN)SelectObject(memDC, cache.headPen);
                        SelectObject(memDC, GetStockObject(NULL_BRUSH));
                        Rectangle(memDC, sr.left, sr.top, sr.right, sr.bottom);
                        SelectObject(memDC, oldPen);
                    }
                    else {
                        // Body
                        HBRUSH bodyBrush = (i % 2 == 0) ? cache.bodyBrush1 : cache.bodyBrush2;
                        FillRect(memDC, &sr, bodyBrush);
                        oldPen = (HPEN)SelectObject(memDC, cache.bodyPen);
                        SelectObject(memDC, GetStockObject(NULL_BRUSH));
                        Rectangle(memDC, sr.left, sr.top, sr.right, sr.bottom);
                        SelectObject(memDC, oldPen);
                    }
                }

                // Score text
                std::wstring scoreTxt = L"Score: " + std::to_wstring(snap.score);
                if (snap.gameOver) scoreTxt += L"    (Press R to restart)";

                SetBkMode(memDC, TRANSPARENT);
                HFONT oldf = (HFONT)SelectObject(memDC, cache.scoreFont);

                // Shadow
                SetTextColor(memDC, RGB(30, 30, 30));
                TextOutW(memDC, 13, GRID_H * CELL + 9, scoreTxt.c_str(), (int)scoreTxt.size());
                // Main text
                SetTextColor(memDC, RGB(230, 230, 230));
                TextOutW(memDC, 12, GRID_H * CELL + 8, scoreTxt.c_str(), (int)scoreTxt.size());

                SelectObject(memDC, oldf);

                // Paused overlay (semi-transparent)
                if (snap.paused && snap.started) {
                    // Fast semi-transparent overlay
                    RECT rr = { 0, 0, GRID_W * CELL, GRID_H * CELL };
                    HBRUSH darkBrush = CreateSolidBrush(RGB(0, 0, 0));
                    // Draw multiple thin overlays for transparency effect
                    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 100, 0 };
                    for (int i = 0; i < 3; i++) {
                        FillRect(memDC, &rr, darkBrush);
                    }
                    DeleteObject(darkBrush);

                    int pauseFontSize = max(32, min(48, (GRID_W * CELL) / 10));
                    HFONT pauseFont = CreateFontW(pauseFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    HFONT oldPause = (HFONT)SelectObject(memDC, pauseFont);
                    SetTextColor(memDC, RGB(220, 220, 220));
                    int pauseY = max(60, GRID_H * CELL / 6);
                    RECT tr = { 0, pauseY, GRID_W * CELL, pauseY + pauseFontSize + 20 };
                    DrawTextW(memDC, L"PAUSED", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(memDC, oldPause);
                    DeleteObject(pauseFont);

                    // Pause menu buttons
                    int buttonFontSize = max(18, min(24, (GRID_W * CELL) / 18));
                    HFONT buttonFont = CreateFontW(buttonFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    HFONT oldButton = (HFONT)SelectObject(memDC, buttonFont);

                    int buttonWidth = max(140, min(200, GRID_W * CELL - 100));
                    int buttonHeight = max(35, min(50, GRID_H * CELL / 9));
                    int centerX = (GRID_W * CELL) / 2;
                    int startY = max(140, pauseY + pauseFontSize + 60);
                    int buttonSpacing = max(50, buttonHeight + 15);

                    // Resume button
                    RECT resumeRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                    HBRUSH resumeBrush = CreateSolidBrush(snap.pauseSelection == 0 ? RGB(50, 200, 50) : RGB(40, 170, 40));
                    FillRect(memDC, &resumeRect, resumeBrush);
                    DeleteObject(resumeBrush);
                    HPEN buttonPen = CreatePen(PS_SOLID, snap.pauseSelection == 0 ? 3 : 2, RGB(90, 220, 90));
                    HPEN oldPen = (HPEN)SelectObject(memDC, buttonPen);
                    SelectObject(memDC, GetStockObject(NULL_BRUSH));
                    Rectangle(memDC, resumeRect.left, resumeRect.top, resumeRect.right, resumeRect.bottom);
                    SelectObject(memDC, oldPen);
                    DeleteObject(buttonPen);
                    SetTextColor(memDC, RGB(220, 220, 220));
                    DrawTextW(memDC, L"Resume", -1, &resumeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    // Menu button
                    startY += buttonSpacing;
                    RECT menuRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                    HBRUSH menuBrush = CreateSolidBrush(snap.pauseSelection == 1 ? RGB(200, 50, 50) : RGB(170, 40, 40));
                    FillRect(memDC, &menuRect, menuBrush);
                    DeleteObject(menuBrush);
                    buttonPen = CreatePen(PS_SOLID, snap.pauseSelection == 1 ? 3 : 2, RGB(220, 90, 90));
                    oldPen = (HPEN)SelectObject(memDC, buttonPen);
                    SelectObject(memDC, GetStockObject(NULL_BRUSH));
                    Rectangle(memDC, menuRect.left, menuRect.top, menuRect.right, menuRect.bottom);
                    SelectObject(memDC, oldPen);
                    DeleteObject(buttonPen);
                    SetTextColor(memDC, RGB(220, 220, 220));
                    DrawTextW(memDC, L"Main Menu", -1, &menuRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(memDC, oldButton);
                    DeleteObject(buttonFont);
                }

                // Not started overlay (semi-transparent)
                if (!snap.started) {
                    // Fast semi-transparent overlay
                    RECT rr = { 0, 0, GRID_W * CELL, GRID_H * CELL };
                    HBRUSH darkBrush = CreateSolidBrush(RGB(0, 0, 0));
                    // Draw overlay twice for lighter darkness
                    for (int i = 0; i < 2; i++) {
                        FillRect(memDC, &rr, darkBrush);
                    }
                    DeleteObject(darkBrush);

                    int startFontSize = max(24, min(36, (GRID_W * CELL) / 12));
                    HFONT startFont = CreateFontW(startFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    HFONT oldStart = (HFONT)SelectObject(memDC, startFont);
                    SetTextColor(memDC, RGB(220, 220, 220));
                    RECT tr = { 0, GRID_H * CELL / 2 - 60, GRID_W * CELL, GRID_H * CELL / 2 };
                    DrawTextW(memDC, L"SNAKE", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    int instructionFontSize = max(14, min(20, (GRID_W * CELL) / 22));
                    HFONT smallFont = CreateFontW(instructionFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    SelectObject(memDC, smallFont);
                    RECT tr2 = { 0, GRID_H * CELL / 2 + 10, GRID_W * CELL, GRID_H * CELL / 2 + 50 };
                    DrawTextW(memDC, L"Press any arrow key to start", -1, &tr2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(memDC, oldStart);
                    DeleteObject(startFont);
                    DeleteObject(smallFont);
                }

                // Game over overlay
                if (snap.gameOver) {
                    RECT rr = { 0, 0, GRID_W * CELL, GRID_H * CELL };
                    // Semi-transparent effect (simple overlay)
                    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 128, 0 };
                    HBRUSH tempOver = CreateSolidBrush(RGB(0, 0, 0));
                    FillRect(memDC, &rr, tempOver);
                    DeleteObject(tempOver);

                    int gameOverFontSize = max(32, min(48, (GRID_W * CELL) / 10));
                    HFONT gameOverFont = CreateFontW(gameOverFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    HFONT oldg = (HFONT)SelectObject(memDC, gameOverFont);
                    SetTextColor(memDC, RGB(220, 220, 220));
                    int titleY = max(40, GRID_H * CELL / 8);
                    RECT tr = { 0, titleY, GRID_W * CELL, titleY + gameOverFontSize + 20 };
                    DrawTextW(memDC, L"GAME OVER", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    // Show score
                    int scoreFontSize = max(18, min(24, (GRID_W * CELL) / 18));
                    HFONT scoreTextFont = CreateFontW(scoreFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    SelectObject(memDC, scoreTextFont);
                    std::wstring scoreTxt = L"Score: " + std::to_wstring(snap.score);
                    int scoreY = titleY + gameOverFontSize + 40;
                    RECT scoreRect = { 0, scoreY, GRID_W * CELL, scoreY + 30 };
                    DrawTextW(memDC, scoreTxt.c_str(), -1, &scoreRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    DeleteObject(scoreTextFont);

                    // Game over menu buttons
                    int buttonFontSize = max(18, min(24, (GRID_W * CELL) / 18));
                    HFONT buttonFont = CreateFontW(buttonFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    SelectObject(memDC, buttonFont);

                    int buttonWidth = max(140, min(200, GRID_W * CELL - 100));
                    int buttonHeight = max(35, min(50, GRID_H * CELL / 9));
                    int centerX = (GRID_W * CELL) / 2;
                    int startY = max(140, scoreY + 50);
                    int buttonSpacing = max(50, buttonHeight + 15);

                    // Restart button
                    RECT restartRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                    HBRUSH restartBrush = CreateSolidBrush(snap.gameOverSelection == 0 ? RGB(50, 200, 50) : RGB(40, 170, 40));
                    FillRect(memDC, &restartRect, restartBrush);
                    DeleteObject(restartBrush);
                    HPEN buttonPen = CreatePen(PS_SOLID, snap.gameOverSelection == 0 ? 3 : 2, RGB(90, 220, 90));
                    HPEN oldPen = (HPEN)SelectObject(memDC, buttonPen);
                    SelectObject(memDC, GetStockObject(NULL_BRUSH));
                    Rectangle(memDC, restartRect.left, restartRect.top, restartRect.right, restartRect.bottom);
                    SelectObject(memDC, oldPen);
                    DeleteObject(buttonPen);
                    SetTextColor(memDC, RGB(220, 220, 220));
                    DrawTextW(memDC, L"Restart", -1, &restartRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    // Menu button
                    startY += buttonSpacing;
                    RECT menuRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                    HBRUSH menuBrush = CreateSolidBrush(snap.gameOverSelection == 1 ? RGB(200, 50, 50) : RGB(170, 40, 40));
                    FillRect(memDC, &menuRect, menuBrush);
                    DeleteObject(menuBrush);
                    buttonPen = CreatePen(PS_SOLID, snap.gameOverSelection == 1 ? 3 : 2, RGB(220, 90, 90));
                    oldPen = (HPEN)SelectObject(memDC, buttonPen);
                    SelectObject(memDC, GetStockObject(NULL_BRUSH));
                    Rectangle(memDC, menuRect.left, menuRect.top, menuRect.right, menuRect.bottom);
                    SelectObject(memDC, oldPen);
                    DeleteObject(buttonPen);
                    SetTextColor(memDC, RGB(220, 220, 220));
                    DrawTextW(memDC, L"Main Menu", -1, &menuRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(memDC, oldg);
                    DeleteObject(gameOverFont);
                    DeleteObject(buttonFont);
                }

                // Win overlay
                if (snap.gameWon) {
                    RECT rr = { 0, 0, GRID_W * CELL, GRID_H * CELL };
                    HBRUSH tempOver = CreateSolidBrush(RGB(0, 0, 0));
                    FillRect(memDC, &rr, tempOver);
                    DeleteObject(tempOver);

                    int winFontSize = max(32, min(48, (GRID_W * CELL) / 10));
                    HFONT winFont = CreateFontW(winFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    HFONT oldg = (HFONT)SelectObject(memDC, winFont);
                    SetTextColor(memDC, RGB(90, 220, 90));
                    int titleY = max(40, GRID_H * CELL / 8);
                    RECT tr = { 0, titleY, GRID_W * CELL, titleY + winFontSize + 20 };
                    DrawTextW(memDC, L"YOU WIN!", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    // Show score
                    int scoreFontSize = max(18, min(24, (GRID_W * CELL) / 18));
                    HFONT scoreTextFont = CreateFontW(scoreFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    SelectObject(memDC, scoreTextFont);
                    std::wstring scoreTxt = L"Perfect Score: " + std::to_wstring(snap.score);
                    int scoreY = titleY + winFontSize + 40;
                    RECT scoreRect = { 0, scoreY, GRID_W * CELL, scoreY + 30 };
                    SetTextColor(memDC, RGB(220, 220, 220));
                    DrawTextW(memDC, scoreTxt.c_str(), -1, &scoreRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    DeleteObject(scoreTextFont);

                    // Win menu buttons
                    int buttonFontSize = max(18, min(24, (GRID_W * CELL) / 18));
                    HFONT buttonFont = CreateFontW(buttonFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    SelectObject(memDC, buttonFont);

                    int buttonWidth = max(140, min(200, GRID_W * CELL - 100));
                    int buttonHeight = max(35, min(50, GRID_H * CELL / 9));
                    int centerX = (GRID_W * CELL) / 2;
                    int startY = max(140, scoreY + 50);
                    int buttonSpacing = max(50, buttonHeight + 15);

                    // Play Again button
                    RECT restartRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                    HBRUSH restartBrush = CreateSolidBrush(snap.gameOverSelection == 0 ? RGB(50, 200, 50) : RGB(40, 170, 40));
                    FillRect(memDC, &restartRect, restartBrush);
                    DeleteObject(restartBrush);
                    HPEN buttonPen = CreatePen(PS_SOLID, snap.gameOverSelection == 0 ? 3 : 2, RGB(90, 220, 90));
                    HPEN oldPen = (HPEN)SelectObject(memDC, buttonPen);
                    SelectObject(memDC, GetStockObject(NULL_BRUSH));
                    Rectangle(memDC, restartRect.left, restartRect.top, restartRect.right, restartRect.bottom);
                    SelectObject(memDC, oldPen);
                    DeleteObject(buttonPen);
                    SetTextColor(memDC, RGB(220, 220, 220));
                    DrawTextW(memDC, L"Play Again", -1, &restartRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    // Menu button
                    startY += buttonSpacing;
                    RECT menuRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                    HBRUSH menuBrush = CreateSolidBrush(snap.gameOverSelection == 1 ? RGB(200, 50, 50) : RGB(170, 40, 40));
                    FillRect(memDC, &menuRect, menuBrush);
                    DeleteObject(menuBrush);
                    buttonPen = CreatePen(PS_SOLID, snap.gameOverSelection == 1 ? 3 : 2, RGB(220, 90, 90));
                    oldPen = (HPEN)SelectObject(memDC, buttonPen);
                    SelectObject(memDC, GetStockObject(NULL_BRUSH));
                    Rectangle(memDC, menuRect.left, menuRect.top, menuRect.right, menuRect.bottom);
                    SelectObject(memDC, oldPen);
                    DeleteObject(buttonPen);
                    SetTextColor(memDC, RGB(220, 220, 220));
                    DrawTextW(memDC, L"Main Menu", -1, &menuRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(memDC, oldg);
                    DeleteObject(winFont);
                    DeleteObject(buttonFont);
                }
            } // end of PLAYING state rendering

            // Blit to screen
            BitBlt(hdc, 0, 0, winW, winH, memDC, 0, 0, SRCCOPY);

            // Cleanup
            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);
            ReleaseDC(g_hwnd, hdc);
        }

        // Frame pacing - maintain consistent frame rate
        auto frameEnd = clock::now();
        auto frameDuration = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart);
        int targetFrameTime = 1000 / TARGET_FPS;
        auto sleepTime = std::chrono::milliseconds(targetFrameTime) - frameDuration;
        if (sleepTime.count() > 0) {
            std::this_thread::sleep_for(sleepTime);
        }
    }
}

//
// Win32 window procedure
//
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN: {
        std::lock_guard<std::mutex> lk(stateMtx);

        if (gameState == MENU) {
            // Menu navigation
            switch (wParam) {
            case VK_UP:
            case 'W':
                menuSelection = (menuSelection - 1 + 3) % 3;
                break;
            case VK_DOWN:
            case 'S':
                menuSelection = (menuSelection + 1) % 3;
                break;
            case VK_RETURN:
            case VK_SPACE:
                // Activate selected menu item
                if (menuSelection == 0) {
                    // Play
                    gameState = PLAYING;
                    resetGameLocked();
                }
                else if (menuSelection == 1) {
                    // Settings
                    gameState = SETTINGS;
                    settingSelection = 0;
                }
                else if (menuSelection == 2) {
                    // Exit
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                }
                break;
            }
        }
        else if (gameState == SETTINGS) {
            // Settings navigation
            switch (wParam) {
            case VK_UP:
            case 'W':
                settingSelection = (settingSelection - 1 + 7) % 7;
                break;
            case VK_DOWN:
            case 'S':
                settingSelection = (settingSelection + 1) % 7;
                break;
            case VK_LEFT:
            case 'A':
                // Decrease value
                if (settingSelection == 0) { // FPS
                    fpsIndex = (fpsIndex - 1 + 4) % 4;
                }
                else if (settingSelection == 1) { // Cell Size
                    cellSize = max(60, cellSize - 5);
                }
                else if (settingSelection == 2) { // Grid Width
                    gridWidth = max(5, gridWidth - 1);
                }
                else if (settingSelection == 3) { // Grid Height
                    gridHeight = max(5, gridHeight - 1);
                }
                else if (settingSelection == 4) { // Speed
                    speedIndex = (speedIndex - 1 + 3) % 3;
                }
                else if (settingSelection == 5) { // Fruit Count
                    fruitCount = max(1, fruitCount - 1);
                }
                break;
            case VK_RIGHT:
            case 'D':
                // Increase value
                if (settingSelection == 0) { // FPS
                    fpsIndex = (fpsIndex + 1) % 4;
                }
                else if (settingSelection == 1) { // Cell Size
                    cellSize = min(120, cellSize + 5);
                }
                else if (settingSelection == 2) { // Grid Width
                    gridWidth = min(40, gridWidth + 1);
                }
                else if (settingSelection == 3) { // Grid Height
                    gridHeight = min(40, gridHeight + 1);
                }
                else if (settingSelection == 4) { // Speed
                    speedIndex = (speedIndex + 1) % 3;
                }
                else if (settingSelection == 5) { // Fruit Count
                    fruitCount = min(15, fruitCount + 1);
                }
                break;
            case VK_RETURN:
            case VK_SPACE:
            case VK_ESCAPE:
                // Back to menu
                if (settingSelection == 6 || wParam == VK_ESCAPE) {
                    gameState = MENU;
                    menuSelection = 0;
                }
                break;
            }
        }
        else {
            // Game controls
            if (paused) {
                // Pause menu navigation
                switch (wParam) {
                case VK_UP:
                case 'W':
                    pauseSelection = (pauseSelection - 1 + 2) % 2;
                    break;
                case VK_DOWN:
                case 'S':
                    pauseSelection = (pauseSelection + 1) % 2;
                    break;
                case VK_RETURN:
                case VK_SPACE:
                case VK_ESCAPE:
                    if (pauseSelection == 0) {
                        // Resume
                        paused = false;
                        lastTickTime = std::chrono::steady_clock::now();
                    }
                    else {
                        // Go to menu
                        gameState = MENU;
                        menuSelection = 0;
                        pauseSelection = 0;
                    }
                    break;
                }
            }
            else if (gameOver) {
                // Game over menu navigation
                switch (wParam) {
                case VK_UP:
                case 'W':
                    gameOverSelection = (gameOverSelection - 1 + 2) % 2;
                    break;
                case VK_DOWN:
                case 'S':
                    gameOverSelection = (gameOverSelection + 1) % 2;
                    break;
                case VK_RETURN:
                case VK_SPACE:
                case VK_ESCAPE:
                    if (gameOverSelection == 0) {
                        // Restart
                        resetGameLocked();
                        gameOverSelection = 0;
                    }
                    else {
                        // Go to menu
                        gameState = MENU;
                        menuSelection = 0;
                        gameOverSelection = 0;
                    }
                    break;
                }
            }
            else if (gameWon) {
                // Win menu navigation (same as game over)
                switch (wParam) {
                case VK_UP:
                case 'W':
                    gameOverSelection = (gameOverSelection - 1 + 2) % 2;
                    break;
                case VK_DOWN:
                case 'S':
                    gameOverSelection = (gameOverSelection + 1) % 2;
                    break;
                case VK_RETURN:
                case VK_SPACE:
                case VK_ESCAPE:
                    if (gameOverSelection == 0) {
                        // Play Again
                        resetGameLocked();
                        gameOverSelection = 0;
                    }
                    else {
                        // Go to menu
                        gameState = MENU;
                        menuSelection = 0;
                        gameOverSelection = 0;
                    }
                    break;
                }
            }
            else {
                // Normal game controls
                switch (wParam) {
                case VK_ESCAPE:
                    // Pause game
                    if (started) {
                        paused = true;
                        pauseSelection = 0;
                    }
                    break;
                case VK_UP:
                case 'W':
                    if (!started) {
                        started = true;
                        lastTickTime = std::chrono::steady_clock::now();
                    }
                    if (dir != DOWN) nextDir = UP;
                    break;
                case VK_DOWN:
                case 'S':
                    if (!started) {
                        started = true;
                        lastTickTime = std::chrono::steady_clock::now();
                    }
                    if (dir != UP) nextDir = DOWN;
                    break;
                case VK_LEFT:
                case 'A':
                    if (!started) {
                        started = true;
                        lastTickTime = std::chrono::steady_clock::now();
                    }
                    if (dir != RIGHT) nextDir = LEFT;
                    break;
                case VK_RIGHT:
                case 'D':
                    if (!started) {
                        started = true;
                        lastTickTime = std::chrono::steady_clock::now();
                    }
                    if (dir != LEFT) nextDir = RIGHT;
                    break;
                case 'R':
                    resetGameLocked();
                    break;
                }
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        std::lock_guard<std::mutex> lk(stateMtx);
        int mouseX = LOWORD(lParam);
        int mouseY = HIWORD(lParam);

        if (gameState == MENU) {
            // Calculate button positions (same as rendering)
            int buttonWidth = max(140, min(220, GRID_W * CELL - 100));
            int buttonHeight = max(35, min(55, GRID_H * CELL / 9));
            int centerX = (GRID_W * CELL) / 2;
            int startY = max(140, (GRID_H * CELL - (3 * buttonHeight + 2 * 65)) / 2);
            int buttonSpacing = max(55, buttonHeight + 15);

            // Play button
            RECT playRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
            if (mouseX >= playRect.left && mouseX <= playRect.right && mouseY >= playRect.top && mouseY <= playRect.bottom) {
                gameState = PLAYING;
                resetGameLocked();
            }

            // Settings button
            startY += buttonSpacing;
            RECT settingsRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
            if (mouseX >= settingsRect.left && mouseX <= settingsRect.right && mouseY >= settingsRect.top && mouseY <= settingsRect.bottom) {
                gameState = SETTINGS;
                settingSelection = 0;
            }

            // Exit button
            startY += buttonSpacing;
            RECT exitRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
            if (mouseX >= exitRect.left && mouseX <= exitRect.right && mouseY >= exitRect.top && mouseY <= exitRect.bottom) {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
        }
        else if (gameState == SETTINGS) {
            // Settings back button
            int rowHeight = max(35, min(50, GRID_H * CELL / 10));
            int startY = max(120, GRID_H * CELL / 4) + rowHeight * 6 + 20;
            RECT backRect = { 0, startY, GRID_W * CELL, startY + 30 };
            if (mouseY >= backRect.top && mouseY <= backRect.bottom) {
                gameState = MENU;
                menuSelection = 0;
            }
        }
        else if (gameState == PLAYING) {
            // Pause menu
            if (paused) {
                int buttonWidth = max(140, min(200, GRID_W * CELL - 100));
                int buttonHeight = max(35, min(50, GRID_H * CELL / 9));
                int centerX = (GRID_W * CELL) / 2;
                int pauseFontSize = max(32, min(48, (GRID_W * CELL) / 10));
                int pauseY = max(60, GRID_H * CELL / 6);
                int startY = max(140, pauseY + pauseFontSize + 60);
                int buttonSpacing = max(50, buttonHeight + 15);

                // Resume button
                RECT resumeRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                if (mouseX >= resumeRect.left && mouseX <= resumeRect.right && mouseY >= resumeRect.top && mouseY <= resumeRect.bottom) {
                    paused = false;
                    lastTickTime = std::chrono::steady_clock::now();
                }

                // Menu button
                startY += buttonSpacing;
                RECT menuRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                if (mouseX >= menuRect.left && mouseX <= menuRect.right && mouseY >= menuRect.top && mouseY <= menuRect.bottom) {
                    gameState = MENU;
                    menuSelection = 0;
                    pauseSelection = 0;
                }
            }
            // Game over menu
            else if (gameOver) {
                int buttonWidth = max(140, min(200, GRID_W * CELL - 100));
                int buttonHeight = max(35, min(50, GRID_H * CELL / 9));
                int centerX = (GRID_W * CELL) / 2;
                int gameOverFontSize = max(32, min(48, (GRID_W * CELL) / 10));
                int titleY = max(40, GRID_H * CELL / 8);
                int scoreY = titleY + gameOverFontSize + 40;
                int startY = max(140, scoreY + 50);
                int buttonSpacing = max(50, buttonHeight + 15);

                // Restart button
                RECT restartRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                if (mouseX >= restartRect.left && mouseX <= restartRect.right && mouseY >= restartRect.top && mouseY <= restartRect.bottom) {
                    resetGameLocked();
                    gameOverSelection = 0;
                }

                // Menu button
                startY += buttonSpacing;
                RECT menuRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                if (mouseX >= menuRect.left && mouseX <= menuRect.right && mouseY >= menuRect.top && mouseY <= menuRect.bottom) {
                    gameState = MENU;
                    menuSelection = 0;
                    gameOverSelection = 0;
                }
            }
            // Win menu
            else if (gameWon) {
                int buttonWidth = max(140, min(200, GRID_W * CELL - 100));
                int buttonHeight = max(35, min(50, GRID_H * CELL / 9));
                int centerX = (GRID_W * CELL) / 2;
                int winFontSize = max(32, min(48, (GRID_W * CELL) / 10));
                int titleY = max(40, GRID_H * CELL / 8);
                int scoreY = titleY + winFontSize + 40;
                int startY = max(140, scoreY + 50);
                int buttonSpacing = max(50, buttonHeight + 15);

                // Play Again button
                RECT restartRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                if (mouseX >= restartRect.left && mouseX <= restartRect.right && mouseY >= restartRect.top && mouseY <= restartRect.bottom) {
                    resetGameLocked();
                    gameOverSelection = 0;
                }

                // Menu button
                startY += buttonSpacing;
                RECT menuRect = { centerX - buttonWidth / 2, startY, centerX + buttonWidth / 2, startY + buttonHeight };
                if (mouseX >= menuRect.left && mouseX <= menuRect.right && mouseY >= menuRect.top && mouseY <= menuRect.bottom) {
                    gameState = MENU;
                    menuSelection = 0;
                    gameOverSelection = 0;
                }
            }
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        // Render thread handles all drawing, just validate
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        running = false;
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

//
// Entry point
//
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInst, PWSTR lpszCmdLine, int nCmdShow) {
    const wchar_t CLASS_NAME[] = L"SnakeSmoothMT";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClassW(&wc);

    int winW = GRID_W * CELL;
    int winH = GRID_H * CELL + WIN_EXTRA_H;

    g_hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Snake - Smooth MT (Optimized)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, winW + 16, winH + 39,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwnd) return 0;

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // Initialize game
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        resetGameLocked();
    }

    // Start threads
    std::thread gameThread(gameThreadFunc);
    std::thread renderThread(renderThreadFunc);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Signal threads to stop
    running = false;

    // Wait for threads with timeout (safety measure)
    if (gameThread.joinable()) gameThread.join();
    if (renderThread.joinable()) renderThread.join();

    return 0;
}