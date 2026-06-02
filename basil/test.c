#define NOB_IMPLEMENTATION
#include "nob.h"
#include "raylib.h"

int add( int a, b )
{
    return a + b;
}

Vector2 *Vector2_add(Vector2 *a, b)
{
    a->x += b.x;
    a->y += b.y;
    return a;
}

float *clamp(float *f, min, max)
{
    *f = (*f < min) ? min : (*f > max) ? max : *f;
    return f;
}

int main( int argc, char **argv )
{
    "%d\n"::printf( argc );

    "%d\n"::printf( -2::add(4) );

    Vector2 pos = {0, 0};
    &pos::Vector2_add( {40, 50} ); // <-- type inferred

    "%g, %g\n"::printf(pos.x, pos.y);

    FLAG_WINDOW_RESIZABLE::SetConfigFlags();
    InitWindow( 800, 450, "Hello, raylib!" );

    Texture tex_test = LoadTexture("test.png");

    GetCurrentMonitor()::GetMonitorRefreshRate()::SetTargetFPS();

    while (!WindowShouldClose())
    {
        float delta_time = GetFrameTime();

        float wscale = GetScreenWidth() / tex_test.width;
        float hscale = GetScreenHeight() / tex_test.height;

        float scale = wscale > hscale ? wscale : hscale;
        if (scale < 1) scale = 1;


        Vector2 input = {
            KEY_RIGHT::IsKeyDown() - KEY_LEFT::IsKeyDown(),
             KEY_DOWN::IsKeyDown() - KEY_UP::IsKeyDown(),
        };

        pos.x += input.x * delta_time * 150;
        pos.y += input.y * delta_time * 150;

        &pos.x::clamp( 0, 400 * scale );
        &pos.y::clamp( 0, 200 * scale );

        BeginDrawing();

        BLACK::ClearBackground();


        float scaled_tex_w = (tex_test.width * scale);
        float scaled_tex_h = (tex_test.height * scale);

        Vector2 bars = {
            .x = GetScreenWidth() - scaled_tex_w,
            .y = GetScreenHeight() - scaled_tex_h,
        };

        tex_test::DrawTexturePro(
            {0, 0, tex_test.width, tex_test.height},
            {0, 0, scaled_tex_w, scaled_tex_h},
            {-bars.x / 2, -bars.y / 2}, 0, WHITE
        );

        "This is so fucking cool holy"::DrawText( pos.x, pos.y, scale * 10, RED );


        DrawFPS(10, 10);

        EndDrawing();
    }

    tex_test::UnloadTexture();

    CloseWindow();
}
