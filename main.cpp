// snake_smooth_mt.cpp
// Smooth-interpolated Snake + multithreaded renderer (Win32 GDI)
// Compile: g++ snake_smooth_mt.cpp -std=c++17 -lgdi32 -o snake.exe
// or use MSVC in a Win32 project.

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
constexpr int WIN_EXTRA_H = 40; // room for score text
constexpr std::chrono::milliseconds TICK_INTERVAL_MS(120); // game tick
constexpr std::chrono::milliseconds RENDER_FRAME_MS(0);   // ~60 FPS

//
// Types
//
struct Pt { int x, y; };
struct FPt { float x, y; };

enum Direction { UP, DOWN, LEFT, RIGHT };

//
// Shared game state (protected with mutex when swapping/snapshotting)
//
static std::deque<Pt> currSnake;   // current (newest) discrete state
static std::deque<Pt> prevSnake;   // previous discrete state (for interpolation)
static Pt food{ 0,0 };
static Direction dir = RIGHT;
static bool gameOver = false;
static int score = 0;

// timing
static std::chrono::steady_clock::time_point lastTickTime = std::chrono::steady_clock::now();
static std::chrono::milliseconds tickDuration = TICK_INTERVAL_MS;

// sync
static std::mutex stateMtx;

// threads control
static std::atomic_bool running{ true };

// RNG
static std::mt19937 rng((unsigned)std::random_device{}());
static std::uniform_int_distribution<int> distW(0, GRID_W - 1);
static std::uniform_int_distribution<int> distH(0, GRID_H - 1);

// window handle (set in WinMain)
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

static bool collides(const std::deque<Pt>& snake, const Pt& p) {
    if (p.x < 0 || p.x >= GRID_W || p.y < 0 || p.y >= GRID_H) return true;
    for (auto& s : snake) if (s.x == p.x && s.y == p.y) return true;
    return false;
}

static void placeFoodLocked() {
    // caller holds stateMtx
    while (true) {
        Pt p{ distW(rng), distH(rng) };
        bool on = false;
        for (auto& s : currSnake) if (s.x == p.x && s.y == p.y) { on = true; break; }
        if (!on) { food = p; break; }
    }
}

//
// Initialize/reset game (call under lock)
//
static void resetGameLocked() {
    currSnake.clear();
    int sx = GRID_W / 2;
    int sy = GRID_H / 2;
    // initial length 3
    currSnake.push_back({ sx, sy });
    currSnake.push_back({ sx - 1, sy });
    currSnake.push_back({ sx - 2, sy });
    prevSnake = currSnake;
    dir = RIGHT;
    gameOver = false;
    score = 0;
    placeFoodLocked();
    lastTickTime = std::chrono::steady_clock::now();
    tickDuration = TICK_INTERVAL_MS;
}

//
// Game tick (discrete update) - runs on game thread
//
static void gameThreadFunc() {
    using clock = std::chrono::steady_clock;
    auto nextTick = clock::now();

    while (running) {
        nextTick += TICK_INTERVAL_MS;
        // do game step
        {
            std::lock_guard<std::mutex> lk(stateMtx);
            if (gameOver) {
                // nothing; keep previous and current the same
            }
            else {
                // move
                Pt head = currSnake.front();
                Pt newHead = moveHead(head, dir);

                // collision check walls
                bool collided = false;
                if (newHead.x < 0 || newHead.x >= GRID_W || newHead.y < 0 || newHead.y >= GRID_H) collided = true;
                else {
                    for (auto& s : currSnake) if (s.x == newHead.x && s.y == newHead.y) { collided = true; break; }
                }

                // snapshot prevSnake for interpolation
                prevSnake = currSnake;

                if (collided) {
                    gameOver = true;
                }
                else {
                    // advance
                    currSnake.push_front(newHead);
                    if (newHead.x == food.x && newHead.y == food.y) {
                        score += 10;
                        // place new food not on snake
                        placeFoodLocked();
                    }
                    else {
                        currSnake.pop_back(); // normal move
                    }
                }
            }

            lastTickTime = clock::now();
            tickDuration = TICK_INTERVAL_MS;
        }

        // request a repaint (optional; render thread will also draw continuously)
        InvalidateRect(g_hwnd, NULL, FALSE);

        // sleep until next tick (but check running frequently)
        while (running && clock::now() < nextTick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

//
// Interpolate helpers
//
static FPt lerp(const FPt& a, const FPt& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

//
// Render snapshot struct to hold copied states for lock-free rendering
//
struct RenderSnapshot {
    std::vector<FPt> prev;  // positions in cell coords
    std::vector<FPt> curr;
    FPt food;
    int score;
    bool gameOver;
    std::chrono::steady_clock::time_point tickTime;
    std::chrono::milliseconds tickDur;
};

//
// The render thread copies state under lock into a snapshot, then renders interpolated frames
//
static void renderThreadFunc() {
    using clock = std::chrono::steady_clock;
    RenderSnapshot snap;

    // target FPS loop
    while (running) {
        // copy current state into snapshot quickly under lock
        {
            std::lock_guard<std::mutex> lk(stateMtx);
            snap.prev.clear();
            snap.curr.clear();

            // copy prevSnake and currSnake positions into vectors of floats
            size_t maxLen = max(prevSnake.size(), currSnake.size());
            snap.prev.reserve(prevSnake.size());
            snap.curr.reserve(currSnake.size());

            for (auto& p : prevSnake) snap.prev.push_back({ float(p.x), float(p.y) });
            for (auto& p : currSnake) snap.curr.push_back({ float(p.x), float(p.y) });

            snap.food = { float(food.x), float(food.y) };
            snap.score = score;
            snap.gameOver = gameOver;
            snap.tickTime = lastTickTime;
            snap.tickDur = tickDuration;
        }

        // compute interpolation alpha in [0,1]
        float alpha = 1.0f;
        {
            auto now = clock::now();
            auto elapsed = now - snap.tickTime;
            float a = float(std::chrono::duration_cast<std::chrono::duration<float>>(elapsed).count() /
                std::chrono::duration_cast<std::chrono::duration<float>>(snap.tickDur).count());
            if (a < 0.f) a = 0.f;
            if (a > 1.f) a = 1.f;
            alpha = a;
        }

        // render using snap and alpha
        // Acquire DC for window and draw double-buffered
        HDC hdc = GetDC(g_hwnd);
        if (hdc) {
            RECT client;
            GetClientRect(g_hwnd, &client);
            int winW = client.right - client.left;
            int winH = client.bottom - client.top;

            // prepare memory DC
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, winW, winH);
            HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);

            // background: soft dark
            HBRUSH bg = CreateSolidBrush(RGB(22, 26, 30));
            FillRect(memDC, &client, bg);
            DeleteObject(bg);

            // grid lines
            HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(40, 40, 48));
            HPEN oldPen = (HPEN)SelectObject(memDC, gridPen);
            for (int x = 0; x <= GRID_W * CELL; x += CELL) {
                MoveToEx(memDC, x, 0, NULL);
                LineTo(memDC, x, GRID_H * CELL);
            }
            for (int y = 0; y <= GRID_H * CELL; y += CELL) {
                MoveToEx(memDC, 0, y, NULL);
                LineTo(memDC, GRID_W * CELL, y);
            }
            SelectObject(memDC, oldPen);
            DeleteObject(gridPen);

            // draw food (single cell)
            HBRUSH foodBrush = CreateSolidBrush(RGB(255, 70, 70));
            RECT fr = {
                int(snap.food.x * CELL),
                int(snap.food.y * CELL),
                int(snap.food.x * CELL + CELL),
                int(snap.food.y * CELL + CELL)
            };
            FillRect(memDC, &fr, foodBrush);
            DeleteObject(foodBrush);

            // draw snake with interpolation
            // We'll interpolate each segment between prev and curr vectors by index.
            // If sizes differ, handle gracefully: if prev shorter use curr pos, etc.
            size_t nSegments = snap.curr.size();
            for (size_t i = 0; i < nSegments; ++i) {
                FPt a, b;
                if (i < snap.prev.size()) a = snap.prev[i];
                else a = snap.curr[i]; // no previous -> stay at current

                b = snap.curr[i];

                FPt ip = lerp(a, b, alpha); // interpolated cell position

                RECT sr = {
                    int(ip.x * CELL) + 1,
                    int(ip.y * CELL) + 1,
                    int(ip.x * CELL + CELL) - 1,
                    int(ip.y * CELL + CELL) - 1
                };

                // head special color
                if (i == 0) {
                    HBRUSH headB = CreateSolidBrush(RGB(90, 220, 90));
                    FillRect(memDC, &sr, headB);
                    DeleteObject(headB);
                    // border
                    HPEN p = CreatePen(PS_SOLID, 1, RGB(0, 110, 0));
                    HPEN oldp = (HPEN)SelectObject(memDC, p);
                    Rectangle(memDC, sr.left, sr.top, sr.right, sr.bottom);
                    SelectObject(memDC, oldp);
                    DeleteObject(p);
                }
                else {
                    // alternating body colors for readability
                    COLORREF fill = (i % 2 == 0) ? RGB(40, 170, 40) : RGB(30, 140, 30);
                    HBRUSH bodyB = CreateSolidBrush(fill);
                    FillRect(memDC, &sr, bodyB);
                    DeleteObject(bodyB);
                    HPEN p = CreatePen(PS_SOLID, 1, RGB(0, 90, 0));
                    HPEN oldp = (HPEN)SelectObject(memDC, p);
                    Rectangle(memDC, sr.left, sr.top, sr.right, sr.bottom);
                    SelectObject(memDC, oldp);
                    DeleteObject(p);
                }
            }

            // Score text (shadow)
            std::wstring scoreTxt = L"Score: " + std::to_wstring(snap.score);
            if (snap.gameOver) scoreTxt += L"    (R to restart)";

            SetBkMode(memDC, TRANSPARENT);
            HFONT hf = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT oldf = (HFONT)SelectObject(memDC, hf);

            // shadow
            SetTextColor(memDC, RGB(30, 30, 30));
            TextOutW(memDC, 12 + 1, GRID_H * CELL + 8 + 1, scoreTxt.c_str(), (int)scoreTxt.size());
            // main
            SetTextColor(memDC, RGB(230, 230, 230));
            TextOutW(memDC, 12, GRID_H * CELL + 8, scoreTxt.c_str(), (int)scoreTxt.size());

            SelectObject(memDC, oldf);
            DeleteObject(hf);

            // game over overlay if needed
            if (snap.gameOver) {
                // translucent dark rectangle (alpha emulation by filling with dark and leaving text)
                HBRUSH over = CreateSolidBrush(RGB(0, 0, 0));
                RECT rr = { 0, 0, GRID_W * CELL, GRID_H * CELL };
                FillRect(memDC, &rr, over);
                DeleteObject(over);

                HFONT gof = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT oldg = (HFONT)SelectObject(memDC, gof);
                SetTextColor(memDC, RGB(220, 220, 220));
                RECT tr = { 0, GRID_H * CELL / 2 - 40, GRID_W * CELL, GRID_H * CELL / 2 + 40 };
                DrawTextW(memDC, L"GAME OVER", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(memDC, oldg);
                DeleteObject(gof);
            }

            // Blit
            BitBlt(hdc, 0, 0, winW, winH, memDC, 0, 0, SRCCOPY);

            // cleanup
            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);

            ReleaseDC(g_hwnd, hdc);
        }

        // frame pacing
        std::this_thread::sleep_for(RENDER_FRAME_MS);
    }
}

//
// Win32 event handling (window owns minimal logic; input modifies direction under lock)
//

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN: {
        std::lock_guard<std::mutex> lk(stateMtx);
        switch (wParam) {
        case VK_UP:
        case 'W': if (dir != DOWN) dir = UP; break;
        case VK_DOWN:
        case 'S': if (dir != UP) dir = DOWN; break;
        case VK_LEFT:
        case 'A': if (dir != RIGHT) dir = LEFT; break;
        case VK_RIGHT:
        case 'D': if (dir != LEFT) dir = RIGHT; break;
        case 'R':
            resetGameLocked();
            break;
        }
        break;
    }
    case WM_DESTROY:
        running = false; // signal threads to stop
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_SIZE:
        // redraw on resize
        InvalidateRect(hwnd, NULL, TRUE);
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
        0, CLASS_NAME, L"Snake - Smooth (multithreaded)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, winW + 16, winH + 39,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwnd) return 0;

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // initial state
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        resetGameLocked();
    }

    // start threads
    std::thread gameThread(gameThreadFunc);
    std::thread renderThread(renderThreadFunc);

    // message loop (main thread used for input & lifetime)
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // shut down threads
    running = false;
    if (gameThread.joinable()) gameThread.join();
    if (renderThread.joinable()) renderThread.join();

    return 0;
}
