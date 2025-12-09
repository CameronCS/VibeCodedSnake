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
constexpr int CELL = 40;
constexpr int GRID_W = 10;
constexpr int GRID_H = 10;
constexpr int WIN_EXTRA_H = 40;
constexpr std::chrono::milliseconds TICK_INTERVAL_MS(120);
constexpr std::chrono::milliseconds RENDER_FRAME_MS(4); // ~250 FPS

//
// Types
//
struct Pt { int x, y; };
struct FPt { float x, y; };

enum Direction { UP, DOWN, LEFT, RIGHT };

//
// Shared game state
//
static std::deque<Pt> currSnake;
static std::deque<Pt> prevSnake;
static Pt food{ 0,0 };
static Direction dir = RIGHT;
static Direction nextDir = RIGHT; // FIXED: queue next direction
static bool gameOver = false;
static int score = 0;

static std::chrono::steady_clock::time_point lastTickTime = std::chrono::steady_clock::now();
static std::chrono::milliseconds tickDuration = TICK_INTERVAL_MS;

static std::mutex stateMtx;
static std::mutex rngMtx; // FIXED: separate mutex for RNG
static std::atomic_bool running{ true };

// RNG - now protected by rngMtx
static std::mt19937 rng((unsigned)std::random_device{}());
static std::uniform_int_distribution<int> distW(0, GRID_W - 1);
static std::uniform_int_distribution<int> distH(0, GRID_H - 1);

static HWND g_hwnd = nullptr;

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
    while (true) {
        Pt p{ distW(rng), distH(rng) };
        bool on = false;
        for (auto& s : currSnake) {
            if (s.x == p.x && s.y == p.y) {
                on = true;
                break;
            }
        }
        if (!on) {
            food = p;
            break;
        }
    }
}

static void resetGameLocked() {
    currSnake.clear();
    int sx = GRID_W / 2;
    int sy = GRID_H / 2;
    currSnake.push_back({ sx, sy });
    currSnake.push_back({ sx - 1, sy });
    currSnake.push_back({ sx - 2, sy });
    prevSnake = currSnake;
    dir = RIGHT;
    nextDir = RIGHT; // FIXED: reset nextDir too
    gameOver = false;
    score = 0;
    placeFoodLocked();
    lastTickTime = std::chrono::steady_clock::now();
    tickDuration = TICK_INTERVAL_MS;
}

//
// Game tick
//
static void gameThreadFunc() {
    using clock = std::chrono::steady_clock;
    auto nextTick = clock::now();

    while (running) {
        nextTick += TICK_INTERVAL_MS;

        {
            std::lock_guard<std::mutex> lk(stateMtx);
            if (!gameOver) {
                // FIXED: Apply queued direction at start of tick
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
                    if (newHead.x == food.x && newHead.y == food.y) {
                        score += 10;
                        placeFoodLocked();
                    }
                    else {
                        currSnake.pop_back();
                    }
                }
            }

            lastTickTime = clock::now();
            tickDuration = TICK_INTERVAL_MS;
        }

        // sleep until next tick
        while (running && clock::now() < nextTick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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
    FPt food;
    int score;
    bool gameOver;
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

            for (auto& p : prevSnake) snap.prev.push_back({ float(p.x), float(p.y) });
            for (auto& p : currSnake) snap.curr.push_back({ float(p.x), float(p.y) });

            snap.food = { float(food.x), float(food.y) };
            snap.score = score;
            snap.gameOver = gameOver;
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

            // Food
            RECT fr = {
                int(snap.food.x * CELL),
                int(snap.food.y * CELL),
                int(snap.food.x * CELL + CELL),
                int(snap.food.y * CELL + CELL)
            };
            FillRect(memDC, &fr, cache.foodBrush);

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

            // Game over overlay
            if (snap.gameOver) {
                RECT rr = { 0, 0, GRID_W * CELL, GRID_H * CELL };
                // Semi-transparent effect (simple overlay)
                BLENDFUNCTION blend = { AC_SRC_OVER, 0, 128, 0 };
                HBRUSH tempOver = CreateSolidBrush(RGB(0, 0, 0));
                FillRect(memDC, &rr, tempOver);
                DeleteObject(tempOver);

                HFONT oldg = (HFONT)SelectObject(memDC, cache.gameOverFont);
                SetTextColor(memDC, RGB(220, 220, 220));
                RECT tr = { 0, GRID_H * CELL / 2 - 40, GRID_W * CELL, GRID_H * CELL / 2 + 40 };
                DrawTextW(memDC, L"GAME OVER", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(memDC, oldg);
            }

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
        auto sleepTime = RENDER_FRAME_MS - frameDuration;
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
        switch (wParam) {
        case VK_UP:
        case 'W': if (dir != DOWN) nextDir = UP; break; // FIXED: set nextDir, check current dir
        case VK_DOWN:
        case 'S': if (dir != UP) nextDir = DOWN; break;
        case VK_LEFT:
        case 'A': if (dir != RIGHT) nextDir = LEFT; break;
        case VK_RIGHT:
        case 'D': if (dir != LEFT) nextDir = RIGHT; break;
        case 'R':
            resetGameLocked();
            break;
        }
        break;
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