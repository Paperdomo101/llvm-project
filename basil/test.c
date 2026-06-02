#define NOB_IMPLEMENTATION
#include "nob.h"
#include "raylib.h"
#include "raymath.h"

#define IsHeld IsKeyDown

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
    a := 20;
    b := "hi!\n";

    "%d\n"::printf( argc );

    "%d\n"::printf( -2::add(4) );

    pos := (Vector2){0, 0};
    &pos::Vector2_add( {40, 50} ); // <-- type inferred

    struct Point { int x; int y } p = { 2, 3 };

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


        Vector2 positive = { KEY_RIGHT::IsHeld(), KEY_DOWN::IsHeld() };
        Vector2 negative = { KEY_LEFT ::IsHeld(), KEY_UP  ::IsHeld() };

        Vector2 input = positive::Vector2Subtract( negative );
        Vector2 input_norm = input::Vector2Normalize();

        Vector2 vel = input_norm::Vector2Scale( 150 * scale );

        pos = pos::Vector2Add( vel::Vector2Scale(delta_time) )
                 ::Vector2Clamp( {0, 0}, {400 * scale, 200 * scale} );

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


        DrawLineEx( pos, pos::Vector2Add(vel), scale, YELLOW );

        DrawFPS(10, 10);

        EndDrawing();
    }

    tex_test::UnloadTexture();

    CloseWindow();
}
