#define NOB_IMPLEMENTATION
#include "nob.h"
#include "raylib.h"


int add(int a, int b)
{
    return a + b;
}

int main( int argc, char **argv )
{
    "%d\n"::printf( argc );

    "%d\n"::printf( 2::add(4) );

    Vector2 pos = {0, 0};

    FLAG_WINDOW_RESIZABLE::SetConfigFlags();
    InitWindow( 800, 450, "Hello, raylib!" );

    GetCurrentMonitor()::GetMonitorRefreshRate()::SetTargetFPS();

    while (!WindowShouldClose())
    {
        float delta_time = GetFrameTime();

        Vector2 input = {
            KEY_RIGHT::IsKeyDown() - KEY_LEFT::IsKeyDown(),
            KEY_DOWN::IsKeyDown() - KEY_UP::IsKeyDown(),
        };

        pos.x += input.x * delta_time * 150;
        pos.y += input.y * delta_time * 150;


        BeginDrawing();

        BLACK::ClearBackground();

        "This is so fucking cool holy"::DrawText( pos.x, pos.y, 30, RED );

        DrawFPS(10, 10);

        EndDrawing();
    }

    CloseWindow();
}
