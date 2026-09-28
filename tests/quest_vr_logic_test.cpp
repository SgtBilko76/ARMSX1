// Desktop checks for the Quest VR logic that can be reasoned about without a headset: the Touch
// controller -> PlayStation pad mapping and the laser pointer's hit on the virtual screen.

#include "frontend/vr/quest_pad_mapper.h"
#include "frontend/vr/screen_geometry.h"
#include "frontend/vr/stereo_reproject.h"

#include <cmath>
#include <cstdio>

using namespace armsx::vr;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

bool Near(float a, float b, float tolerance = 1e-3f) {
    return std::fabs(a - b) <= tolerance;
}

void TestFaceButtonsAndShoulders() {
    QuestPadMapper mapper;
    ControllerInput in;
    in.a = in.b = in.x = in.y = true;
    in.left_grip = in.right_grip = 1.0f;
    in.left_trigger = in.right_trigger = 1.0f;
    const MapperOutput out = mapper.Update(in, 0.0);
    const std::uint32_t expected = PAD_CROSS | PAD_CIRCLE | PAD_SQUARE | PAD_TRIANGLE | PAD_L1 | PAD_R1 | PAD_L2 | PAD_R2;
    Check(out.pad.buttons == expected, "A/B/X/Y, grips and triggers map to face buttons and shoulders");
    Check(!out.open_menu && !out.recenter, "plain buttons raise no events");
}

void TestTriggerHysteresis() {
    QuestPadMapper mapper;
    ControllerInput in;
    in.right_trigger = 0.5f;
    Check(!(mapper.Update(in, 0.0).pad.buttons & PAD_R2), "half trigger does not press R2");
    in.right_trigger = 0.7f;
    Check(mapper.Update(in, 0.1).pad.buttons & PAD_R2, "trigger past 0.6 presses R2");
    in.right_trigger = 0.5f;
    Check(mapper.Update(in, 0.2).pad.buttons & PAD_R2, "R2 holds between the thresholds");
    in.right_trigger = 0.3f;
    Check(!(mapper.Update(in, 0.3).pad.buttons & PAD_R2), "R2 releases under 0.4");
}

void TestStickClicksAreStartSelect() {
    QuestPadMapper mapper;
    ControllerInput in;
    in.left_stick_click = true;
    in.right_stick_click = true;
    const MapperOutput out = mapper.Update(in, 0.0);
    Check((out.pad.buttons & (PAD_START | PAD_SELECT)) == (PAD_START | PAD_SELECT), "stick clicks are Start and Select");
    Check(!(out.pad.buttons & (PAD_L3 | PAD_R3)), "stick clicks alone are not L3/R3");
}

void TestMenuTapOpensMenu() {
    QuestPadMapper mapper;
    ControllerInput in;
    in.menu = true;
    Check(!mapper.Update(in, 0.0).open_menu, "menu press does not open yet");
    in.menu = false;
    Check(mapper.Update(in, 0.2).open_menu, "menu tap opens the menu on release");

    in.menu = true;
    mapper.Update(in, 1.0);
    in.menu = false;
    Check(!mapper.Update(in, 2.0).open_menu, "long menu press does nothing");
}

void TestMenuChords() {
    QuestPadMapper mapper;
    ControllerInput in;
    in.menu = true;
    mapper.Update(in, 0.0);

    in.left_stick_click = true;
    MapperOutput out = mapper.Update(in, 0.05);
    Check((out.pad.buttons & PAD_L3) && !(out.pad.buttons & PAD_START), "menu + left stick click is L3, not Start");

    in.right_trigger = 1.0f;
    out = mapper.Update(in, 0.1);
    Check(out.recenter, "menu + right trigger recenters");
    Check(!(out.pad.buttons & PAD_R2), "the recenter trigger is not R2");

    // Releasing Menu first must not turn the chord partners into their plain buttons.
    in.menu = false;
    out = mapper.Update(in, 0.15);
    Check(!out.open_menu, "a chord does not also open the menu");
    Check(!(out.pad.buttons & PAD_R2) && !(out.pad.buttons & PAD_START), "chord buttons stay claimed after Menu is released");

    in.left_stick_click = false;
    in.right_trigger = 0.0f;
    mapper.Update(in, 0.2);
    in.right_trigger = 1.0f;
    Check(mapper.Update(in, 0.3).pad.buttons & PAD_R2, "the trigger is R2 again once released");
}

void TestLeftStickDpad() {
    QuestPadMapper mapper;
    ControllerInput in;
    in.left_x = 0.5f;
    MapperOutput out = mapper.Update(in, 0.0);
    Check(!(out.pad.buttons & PAD_RIGHT) && out.pad.left_x > 0.0f, "a gentle push is analog only");

    in.left_x = 0.9f;
    out = mapper.Update(in, 0.1);
    Check((out.pad.buttons & PAD_RIGHT) && out.pad.left_x == 0.0f, "a hard push is the D-pad instead of analog");

    in.left_x = 0.6f;
    Check(mapper.Update(in, 0.2).pad.buttons & PAD_RIGHT, "the D-pad holds above the release threshold");
    in.left_x = 0.3f;
    Check(!(mapper.Update(in, 0.3).pad.buttons & PAD_RIGHT), "the D-pad releases below it");

    in.left_y = 0.9f;
    Check(mapper.Update(in, 0.4).pad.buttons & PAD_UP, "stick up is D-pad up");

    // Menu + B switches to analog only.
    in = ControllerInput();
    mapper.Update(in, 0.5);
    in.menu = true;
    mapper.Update(in, 0.6);
    in.b = true;
    out = mapper.Update(in, 0.7);
    Check(!(out.pad.buttons & PAD_CIRCLE), "menu + B is not Circle");
    Check(!mapper.DpadMode(), "menu + B turns the stick D-pad off");
    in = ControllerInput();
    mapper.Update(in, 0.8);
    in.left_x = 1.0f;
    out = mapper.Update(in, 0.9);
    Check(!(out.pad.buttons & PAD_RIGHT) && Near(out.pad.left_x, 1.0f), "analog-only mode keeps the full push analog");
}

void TestAnalogBytes() {
    Check(AnalogByte(0.0f, false) == 0x80, "centre is 0x80");
    Check(AnalogByte(-1.0f, false) == 0x00 && AnalogByte(1.0f, false) == 0xFF, "full left/right");
    Check(AnalogByte(1.0f, true) == 0x00, "stick up is 0x00 on the PlayStation");
}

void TestResetKeepsDpadMode() {
    QuestPadMapper mapper;
    ControllerInput in;
    in.menu = true;
    mapper.Update(in, 0.0);
    in.b = true;
    mapper.Update(in, 0.1);
    mapper.ResetHeld();
    Check(!mapper.DpadMode(), "ResetHeld keeps the D-pad mode choice");
    in = ControllerInput();
    Check(mapper.Update(in, 0.2).pad.buttons == 0, "ResetHeld releases everything");
}

void TestScreenGeometry() {
    for (const bool curved : {true, false}) {
        ScreenGeometry screen;
        screen.curved = curved;
        screen.head = {0.3f, 1.6f, -0.2f};
        screen.yaw = 0.7f;

        const Vec3 forward = Rotate(YawQuat(screen.yaw), {0.0f, 0.0f, -1.0f});
        PointerHit hit = screen.Intersect(screen.head, forward);
        Check(hit.valid && Near(hit.u, 0.5f) && Near(hit.v, 0.5f), "looking straight ahead hits the centre");

        const Vec3 backward{-forward.x, -forward.y, -forward.z};
        Check(!screen.Intersect(screen.head, backward).valid, "pointing away misses");

        // A point a little right of and above centre, in the screen's own frame.
        const Vec3 local_dir{0.2f, 0.1f, -1.0f};
        const Vec3 dir = Rotate(YawQuat(screen.yaw), local_dir);
        hit = screen.Intersect(screen.head, dir);
        Check(hit.valid && hit.u > 0.5f && hit.v < 0.5f, "right and up maps to larger u, smaller v");

        const Vec3 far_left = Rotate(YawQuat(screen.yaw), {-3.0f, 0.0f, -1.0f});
        Check(!screen.Intersect(screen.head, far_left).valid, "past the left edge misses");
    }

    // Edge check on the curve: aiming at the chord's end lands exactly on u = 1.
    ScreenGeometry screen;
    const float half_angle = screen.CentralAngle() * 0.5f;
    const Vec3 edge{std::sin(half_angle) * 0.999f, 0.0f, -std::cos(half_angle)};
    const PointerHit hit = screen.Intersect({}, edge);
    Check(hit.valid && hit.u > 0.99f, "the curve's right edge is u = 1");

    Check(Near(YawOf(YawQuat(1.2f)), 1.2f), "YawOf inverts YawQuat");
    const Vec3 centre = screen.QuadCentre();
    Check(Near(centre.z, -screen.distance_m), "the flat screen sits distance_m ahead");
}

void TestStereoReprojection() {
    constexpr int kWidth = 16;
    StereoParams params;
    params.max_disparity_px = 4.0f;
    params.convergence = 1.0f;
    params.scale = 4;
    StereoReprojector reprojector;

    std::uint16_t color[kWidth];
    float depth[kWidth];
    for (int x = 0; x < kWidth; x++) {
        color[x] = static_cast<std::uint16_t>(x + 1);
        depth[x] = -1.0f;
    }
    std::uint16_t out[kWidth * 4];

    // Flat content is the same in both eyes: every source pixel repeated `scale` times.
    for (const int eye : {-1, 1}) {
        reprojector.ReprojectRow(color, depth, kWidth, eye, params, out);
        bool same = true;
        for (int o = 0; o < kWidth * 4; o++) {
            same = same && out[o] == color[o / 4];
        }
        Check(same, "flat pixels are not shifted");
    }

    Check(Near(Disparity(0.0f, params), 4.0f), "far away is the full separation");
    Check(Near(Disparity(1.0f, params), 0.0f), "w = 1/C sits on the screen");
    Check(Near(Disparity(10.0f, params), -4.0f), "very near clamps to -K");
    Check(Near(Disparity(-1.0f, params), 0.0f), "flat is on the screen");

    // A far row (w = 0) moves K/2 = 2 source pixels: left in the left eye, right in the right.
    for (int x = 0; x < kWidth; x++) {
        depth[x] = 0.0f;
    }
    reprojector.ReprojectRow(color, depth, kWidth, -1, params, out);
    Check(out[6 * 4] == color[8], "far pixels shift left in the left eye");
    reprojector.ReprojectRow(color, depth, kWidth, 1, params, out);
    Check(out[10 * 4] == color[8], "far pixels shift right in the right eye");

    // A flat pixel in front keeps its place; the far pixel landing on it is hidden.
    depth[10] = -1.0f;
    reprojector.ReprojectRow(color, depth, kWidth, 1, params, out);
    bool flat_kept = true;
    for (int o = 10 * 4; o < 11 * 4; o++) {
        flat_kept = flat_kept && out[o] == color[10];
    }
    Check(flat_kept, "a nearer pixel occludes a farther one landing on it");

    // No holes: every output sample carries some source colour.
    bool filled = true;
    for (int o = 0; o < kWidth * 4; o++) {
        filled = filled && out[o] != 0;
    }
    Check(filled, "revealed background is filled");

    Check(Near(MaxDisparityPx(1.0f, 1.8f, 320), 0.063f / 1.8f * 320.0f), "K is the eye distance in image pixels");
    Check(MaxDisparityPx(0.0f, 1.8f, 320) == 0.0f, "zero strength is mono");
}

} // namespace

int main() {
    TestFaceButtonsAndShoulders();
    TestTriggerHysteresis();
    TestStickClicksAreStartSelect();
    TestMenuTapOpensMenu();
    TestMenuChords();
    TestLeftStickDpad();
    TestAnalogBytes();
    TestResetKeepsDpadMode();
    TestScreenGeometry();
    TestStereoReprojection();

    if (g_failures) {
        std::fprintf(stderr, "quest_vr_logic: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("quest_vr_logic: ok\n");
    return 0;
}
