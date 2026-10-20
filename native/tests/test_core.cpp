// Offline contract tests for the engine-independent core (S0 gate: `build.bat core test`).

#include "core/binary_manifest.h"
#include "core/combat_map.h"
#include "core/frame_map.h"
#include "core/input_map.h"
#include "core/pose_ring.h"
#include "core/projection.h"
#include "core/protocol.h"
#include "core/terrain_kinds.h"
#include "core/terrain_merge.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            std::printf("[lie_tests] FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                              \
        }                                                                              \
    } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) <= eps; }

static void test_frame_map() {
    const lie::FrameMap m = lie::FrameMap::rebase(0, 12345.5, -2330.0, 870.0, 220.0);
    CHECK(m.epoch == 1 && m.valid());
    for (double y : {-50000.0, -1.25, 0.0, 12345.5, 99999.75}) {
        CHECK(m.host_y(m.guest_y(y)) == y); // exact round trip
        CHECK(m.host_z(m.guest_z(y)) == y);
    }
    CHECK(m.guest_y(12345.5) == 870.0 && m.guest_z(-2330.0) == 220.0);
    // feet aligned with different capsules (EM 70, EL 65)
    const double lilac_z = -2330.0 + 70.0;
    const double lily_z = m.guest_actor_z(lilac_z, 70.0, 65.0);
    CHECK(near(lily_z - 65.0, 220.0));
    CHECK(near(m.host_actor_z(lily_z, 65.0, 70.0), lilac_z));
    // recenter: Lily and boxes shift together, host coordinates unchanged
    const lie::FrameMap r = m.recentered(-40000.0, 5.0);
    CHECK(r.epoch == 2);
    const double host_y = 15000.0;
    CHECK(near(r.guest_y(host_y), m.guest_y(host_y) - 40000.0));
    CHECK(near(r.host_y(m.guest_y(host_y) - 40000.0), host_y));
}

static void test_seqlock_and_ring() {
    auto ring = std::make_unique<lie::SpscRing<lie::Event, 8>>();
    for (int i = 0; i < 8; ++i) CHECK(ring->push(lie::Event::make(lie::EventType::Restore, lie::LethalPayload{i})));
    CHECK(!ring->push(lie::Event::make(lie::EventType::Restore, lie::LethalPayload{99}))); // full: newest rejected
    CHECK(ring->dropped.load() == 1 && ring->size() == 8);
    lie::Event e;
    for (int i = 0; i < 8; ++i) {
        CHECK(ring->pop(e) && e.as<lie::LethalPayload>().damage_el == i); // FIFO order kept
    }
    CHECK(!ring->pop(e));
    for (int i = 0; i < 20; ++i) { // wrap-around
        CHECK(ring->push(lie::Event::make(lie::EventType::Hurt, lie::HurtPayload{static_cast<std::uint32_t>(i), 5, 100})));
        CHECK(ring->pop(e) && e.type == lie::EventType::Hurt && e.as<lie::HurtPayload>().source_id == static_cast<std::uint32_t>(i));
    }
    ring->push(lie::Event{});
    ring->drain();
    CHECK(ring->size() == 0);

    lie::Seqlock<lie::Vitals> v;
    lie::Vitals out;
    CHECK(!v.read(out)); // never written
    v.write([](lie::Vitals& x) { x.hp = 42; x.max_hp = 400; });
    CHECK(v.read(out) && out.hp == 42 && out.max_hp == 400 && (v.version() & 1u) == 0);
}

static void test_pose_ring() {
    auto ring = std::make_unique<lie::PoseRing>();
    CHECK(!lie::pose::sample(*ring, 1.0).valid);
    auto pub = [&](std::uint32_t epoch, double t, double y) {
        lie::PoseSample s;
        s.epoch = epoch;
        s.t = t;
        s.guest_y = y;
        s.host_y = y + 1000.0;
        lie::pose::publish(*ring, s);
    };
    for (int i = 0; i < 10; ++i) pub(1, 1.0 + i / 60.0, i * 10.0);
    auto mid = lie::pose::sample(*ring, 1.0 + 4.5 / 60.0);
    CHECK(mid.valid && near(mid.pose.guest_y, 45.0, 1e-6) && near(mid.pose.host_y, 1045.0, 1e-6));
    CHECK(near(lie::pose::sample(*ring, 9.0).pose.guest_y, 90.0)); // no extrapolation
    pub(2, 1.0 + 10 / 60.0, 5000.0);                                 // rebase: older epoch ignored
    auto after = lie::pose::sample(*ring, 1.0 + 4.5 / 60.0);
    CHECK(after.valid && after.pose.epoch == 2 && near(after.pose.guest_y, 5000.0));
    for (int i = 0; i < 100; ++i) pub(3, 2.0 + i / 60.0, i); // wrap-around of the 32 slots
    CHECK(near(lie::pose::sample(*ring, 2.0 + 90.25 / 60.0).pose.guest_y, 90.25, 1e-6));
    CHECK(near(lie::pose::sample_delay(0.018, 1.0 / 60.0), 0.0216, 1e-9));
    CHECK(near(lie::pose::sample_delay(0.005, 1.0 / 60.0), 1.0 / 60.0, 1e-12));
    lie::pose::FrameTimer<20> ft;
    for (int i = 0; i <= 20; ++i) ft.tick(i / 60.0);
    CHECK(near(ft.p95(), 1.0 / 60.0, 1e-9));
}

static void test_terrain() {
    using lie::terrain::Face;
    const std::uint64_t A = 1, B = 2, C = 3;
    // floor A [0,100], platform B [300,320]; window [-500, 1000]
    auto iv = lie::terrain::column_intervals({{320, B}, {100, A}}, {{0, A}, {300, B}}, -500, 1000);
    CHECK(iv.size() == 2 && near(iv[0].lo, 0) && near(iv[0].hi, 100) && near(iv[1].lo, 300) && near(iv[1].hi, 320));
    // ceiling C contains the window top (down trace starts inside), floor extends below the window bottom
    iv = lie::terrain::column_intervals({{0, C, true}, {100, A}}, {{-500, A, true}, {800, C}}, -500, 1000);
    CHECK(iv.size() == 2 && near(iv[0].lo, -500) && near(iv[0].hi, 100) && near(iv[1].lo, 800) && near(iv[1].hi, 1000));
    // compound component with a gap: one mesh holding a floor and a ledge
    iv = lie::terrain::column_intervals({{500, A}, {100, A}}, {{0, A}, {450, A}}, -500, 1000);
    CHECK(iv.size() == 2 && near(iv[0].hi, 100) && near(iv[1].lo, 450));
    // touching solids are unioned
    iv = lie::terrain::column_intervals({{200, A}, {100, B}}, {{100, A}, {0, B}}, -500, 1000);
    CHECK(iv.size() == 1 && near(iv[0].lo, 0) && near(iv[0].hi, 200));

    // open shell A (faces at 600 and 100, hollow inside) next to a solid floor B [-100, 0]
    auto comps = lie::terrain::component_intervals({{600, A}, {0, B}}, {{100, A}, {-100, B}}, -500, 1000);
    auto hollow_a = [&](std::uint64_t c, double) { return c != A; };
    iv = lie::terrain::resolve_shells(comps, -500, 1000, hollow_a);
    CHECK(iv.size() == 3 && near(iv[0].lo, -100) && near(iv[0].hi, 0));
    CHECK(near(iv[1].lo, 100) && near(iv[1].hi, 100 + lie::terrain::kShellThickness));
    CHECK(near(iv[2].lo, 600 - lie::terrain::kShellThickness) && near(iv[2].hi, 600));
    // a hollow shell containing the window top keeps only its real (bottom) face
    comps = lie::terrain::component_intervals({{0, A, true}}, {{400, A}}, -500, 1000);
    iv = lie::terrain::resolve_shells(comps, -500, 1000, hollow_a);
    CHECK(iv.size() == 1 && near(iv[0].lo, 400) && near(iv[0].hi, 400 + lie::terrain::kShellThickness));

    // per-band storage: a solid [100, 700] and a platform [800, 820] split over 256-UU bands stitch back exactly
    {
        using lie::terrain::Interval;
        const std::vector<Interval> col{{100, 700}, {800, 820}};
        std::vector<Interval> pieces;
        for (int b = 0; b < 4; ++b) {
            auto p = lie::terrain::clip_intervals(col, b * 256.0, (b + 1) * 256.0);
            pieces.insert(pieces.end(), p.begin(), p.end());
        }
        CHECK(pieces.size() == 4);
        iv = lie::terrain::union_intervals(pieces);
        CHECK(iv.size() == 2 && near(iv[0].lo, 100) && near(iv[0].hi, 700) && near(iv[1].lo, 800) && near(iv[1].hi, 820));
    }

    // flat floor over 10 columns + a 3-column wall -> 2 rectangles
    const double dy = 16.0;
    std::vector<lie::terrain::Column> cols;
    for (int i = 0; i < 10; ++i) cols.push_back({i * dy, {{0.0, 100.0}}});
    for (int i = 6; i < 9; ++i) cols[i].solids.push_back({100.0, 400.0});
    for (auto& c : cols) c.solids = lie::terrain::union_intervals(c.solids);
    auto rects = lie::terrain::merge_columns(cols, dy);
    CHECK(rects.size() == 3); // columns 6..8 are one tall interval [0,400], the rest splits into two floor pieces
    double area = 0.0;
    for (const auto& r : rects) area += 4.0 * r.hy * r.hz;
    CHECK(near(area, 7 * dy * 100.0 + 3 * dy * 400.0, 1e-3));

    // 30-degree ramp over 8 columns becomes fill box + rotated slab
    cols.clear();
    const double step = dy * std::tan(30.0 * 3.14159265358979323846 / 180.0);
    for (int i = 0; i < 8; ++i) cols.push_back({i * dy, {{0.0, 100.0 + i * step}}});
    rects = lie::terrain::merge_columns(cols, dy);
    CHECK(rects.size() == 2);
    const auto& slab = rects[1];
    CHECK(near(slab.roll_deg, 30.0, 0.01));
    // the slab's upper face passes through every column's top
    const double ang = slab.roll_deg * 3.14159265358979323846 / 180.0;
    for (int i = 0; i < 8; ++i) {
        const double y = i * dy;
        const double top_center_y = slab.y - std::sin(ang) * slab.hz;
        const double top_center_z = slab.z + std::cos(ang) * slab.hz;
        const double z_on_face = top_center_z + (y - top_center_y) * std::tan(ang);
        CHECK(near(z_on_face, 100.0 + i * step, 0.05));
    }

    // floating 22-UU plank rising at 19 degrees over 40 columns: no fill box, slab keeps the plank's thickness
    cols.clear();
    const double pstep = dy * std::tan(19.0 * 3.14159265358979323846 / 180.0);
    for (int i = 0; i < 40; ++i) cols.push_back({i * dy, {{300.0 + i * pstep, 322.0 + i * pstep}}});
    rects = lie::terrain::merge_columns(cols, dy);
    CHECK(rects.size() == 1);
    const auto& plank = rects[0];
    const double pang = plank.roll_deg * 3.14159265358979323846 / 180.0;
    for (int i = 0; i < 40; ++i) {
        const double y = i * dy;
        const double bottom_center_y = plank.y + std::sin(pang) * plank.hz;
        const double bottom_center_z = plank.z - std::cos(pang) * plank.hz;
        const double z_on_bottom = bottom_center_z + (y - bottom_center_y) * std::tan(pang);
        CHECK(z_on_bottom >= 300.0 + i * pstep - 0.05);
    }

    // registry: subclasses of AOneWayPlatform must be listed before it (first IsA match wins)
    {
        using namespace lie::terrain;
        auto index_of = [](std::wstring_view cls) {
            for (std::size_t i = 0; i < std::size(kMechanisms); ++i) {
                if (cls == kMechanisms[i].em_class) return static_cast<int>(i);
            }
            return -1;
        };
        const int one_way = index_of(L"/Script/Zion.OneWayPlatform");
        CHECK(one_way >= 0 && index_of(L"/Script/Zion.MovingPlatform_Elevator") < one_way && index_of(L"/Script/Zion.MovingPlatform_Spline") < one_way);
        for (const auto& r : kMechanisms) CHECK((r.flags & lie::kTerrainSolid) != 0);
        CHECK(mirror_of(lie::kTerrainSolid | lie::kTerrainOneWay) == Mirror::OneWay && mirror_of(kStaticFlags) == Mirror::Box);
    }

    std::vector<lie::TerrainRect> many;
    for (int i = 0; i < 20; ++i) many.push_back({static_cast<float>(i * 100), 0, 10, 10, 0, lie::kTerrainSolid});
    lie::terrain::clamp_to_capacity(many, 5, 0.0, 0.0);
    CHECK(many.size() == 5);
    for (const auto& r : many) CHECK(r.y < 500.0f);
}

static void test_combat() {
    CHECK(lie::combat::hurt_to_el(20, 400, 100) == 80);
    CHECK(lie::combat::hurt_to_el(1, 100, 500) == 1);  // never rounds a real hit to zero
    CHECK(lie::combat::hurt_to_el(0, 100, 100) == 0);
    CHECK(lie::combat::hurt_to_el(30, 100, 0) == 30);  // missing reading passes through
    CHECK(lie::combat::hit_to_em(45, 50, 50) == 45);
    CHECK(lie::combat::hit_to_em(100, 250, 200) == 125);
    auto s = lie::combat::split_lethal(50, 120);
    CHECK(!s.lethal && s.apply == 50);
    s = lie::combat::split_lethal(500, 120);
    CHECK(s.lethal && s.apply == 119);
    s = lie::combat::split_lethal(5, 1);
    CHECK(s.lethal && s.apply == 0);
}

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_manifests() {
    std::string err;
    auto el = lie::binary::parse(slurp(std::string(LIE_SOURCE_DIR) + "/data/binary/EL.json"), &err);
    CHECK(el.has_value());
    CHECK(el->module == "EnderLiliesSteam-Win64-Shipping.exe");
    CHECK(el->accepts_sha256("9B939A44457D78AF1F5E8B90EF0F88B95D7DBC35A330350F2EF1E100CDD48329"));
    CHECK(!el->accepts_sha256("34FD2C50091A41FE60484AFBC08DFCFDC23AC68D2834AA93C9058FB7EC270FFA"));
    for (const char* f : {"InputConsume", "GameplayCameraUpdate", "HPComponentDoDamage"}) CHECK(el->fn(f) != nullptr);
    CHECK(el->fn("InputConsume")->rva == 0x00CEF310 && el->fn("InputConsume")->prefix.front() == 0x48);
    CHECK(el->offset("ZenithInputComponent.InputSnapshot").value_or(0) == 224);

    auto em = lie::binary::parse(slurp(std::string(LIE_SOURCE_DIR) + "/data/binary/EM.json"), &err);
    CHECK(em.has_value());
    for (const char* f : {"StatHPComponentDoDamage", "KnockbackComponentStartKnockback"}) CHECK(em->fn(f) != nullptr);

    CHECK(!lie::binary::parse(R"({"schema":1,"module":"x.exe","sha256":["00"]})", &err));
    CHECK(!lie::binary::parse(R"({"schema":1,"module":"x.exe","sha256":[")" + std::string(64, 'a') +
                                  R"("],"functions":{"F":{"rva":"0x10","prefix":"4889"}}})",
                              &err)); // prefix too short to be a safe signature
    lie::json::Value v;
    CHECK(lie::json::parse("{\"a\":[1,2.5,-3e2],\"s\":\"\\u4e2d\\\"\",\"t\":true,\"n\":null}", v));
    CHECK(v.get("a")->arr.size() == 3 && near(v.get("a")->arr[2].num, -300.0) && v.get("s")->str == "\xE4\xB8\xAD\"");
}

static void test_projection() {
    // side-scroller camera looking along +X from 1000 UU in front of the subject
    const lie::proj::Vec3 cam{-1000.0, 500.0, 200.0};
    auto c = lie::proj::project({0.0, 500.0, 200.0}, cam, 0, 0, 0, 90.0, 16.0 / 9.0);
    CHECK(c.valid && near(c.u, 0.5) && near(c.v, 0.5) && near(c.depth, 1000.0));
    auto right = lie::proj::project({0.0, 600.0, 200.0}, cam, 0, 0, 0, 90.0, 16.0 / 9.0);
    auto left = lie::proj::project({0.0, 400.0, 200.0}, cam, 0, 0, 0, 90.0, 16.0 / 9.0);
    auto low = lie::proj::project({0.0, 500.0, 100.0}, cam, 0, 0, 0, 90.0, 16.0 / 9.0);
    CHECK(right.u > 0.5 && left.u < 0.5 && near(left.u + right.u, 1.0) && low.v > 0.5); // +Y is screen right, -Z screen down
    CHECK(near(right.u - 0.5, 0.5 * 100.0 / 1000.0));                                     // tan(45) = 1
    CHECK(near(lie::proj::view_width(1000.0, 90.0), 2000.0));
    // pitched camera: a point on the optical axis stays centred
    auto pitched = lie::proj::project({0.0, 500.0, 200.0 - 1000.0 * std::tan(10.0 * lie::proj::kPi / 180.0)}, cam, -10.0, 0, 0, 90.0, 16.0 / 9.0);
    CHECK(near(pitched.u, 0.5) && near(pitched.v, 0.5, 1e-9));
}

static void test_input_routing() {
    using namespace lie::input;
    auto has = [](std::uint32_t bits, int b) { return (bits & (1u << b)) != 0; };
    // key names as EM's / EL's settings store them
    CHECK(input_from_name("H") == Input{'H'} && input_from_name("F11") == Input{0x7A} && input_from_name("SpaceBar") == Input{vk::Space});
    CHECK(input_from_name("Gamepad_LeftThumbstick") == (Input{0, xb::LThumb}) && input_from_name("Gamepad_RightTrigger") == (Input{0, 0, 2}));
    CHECK(!input_from_name("None").valid() && !input_from_name("").valid() && !input_from_name("F13").valid());
    int names = 0;
    for_each_key_name([&](const char* n) { names += input_from_name(n).valid() ? 1 : 0; });
    CHECK(names == 26 + 12 + static_cast<int>(sizeof(kNamedKeys) / sizeof(kNamedKeys[0])));

    // Lily's actions: the player's EL bindings (this machine's EL settings, read live 2026-10-19) + EM's interact,
    // minus EM's keys (this machine's EM: interact E / L3, minimap R / R3, menu Tab / Back, pause Esc / Start)
    using lie::cmd::Attack, lie::cmd::Dodge, lie::cmd::Heal, lie::cmd::Hook, lie::cmd::Interact, lie::cmd::Jump;
    using lie::cmd::Sub1, lie::cmd::Sub2, lie::cmd::SummonSwitch;
    lie::ElBindings el;
    el.valid = 1;
    auto bind = [&](int c, Input k, Input p) { el.kb[c][0] = k, el.pad[c][0] = p; };
    bind(Jump, {vk::Space}, {0, xb::RB});
    bind(Dodge, {vk::LShift}, {0, xb::LB});
    bind(Attack, {'J'}, {0, xb::X});
    bind(Sub1, {'K'}, {0, xb::Y});
    bind(Sub2, {'L'}, {0, xb::B});
    bind(SummonSwitch, {'Q'}, {0, xb::A});
    bind(Hook, {'P'}, {0, 0, 2});
    bind(Heal, {'R'}, {0, 0, 1});
    bind(Interact, {'F'}, {0, xb::LThumb}); // EL's own interact key never applies: EM's does
    el.kb_move[0][0] = {'W'}, el.kb_move[0][1] = {vk::Up}, el.kb_move[1][0] = {'S'}, el.kb_move[2][0] = {'A'}, el.kb_move[3][0] = {'D'};
    EmWorldKeys em = em_default_world_keys();
    em.interact_pad = {0, xb::LThumb};
    em.reserved[11] = {0, xb::LThumb};
    Bindings b;
    Conflict cf[8];
    const int nconf = compose_bindings(el, em, b, cf, 8);
    CHECK(nconf == 2); // R (heal) is EM's minimap, Up (move) is EM's camera offset
    CHECK(cf[0].cmd == Heal && cf[0].in == Input{'R'} && std::string(kEmReserved[cf[0].reserved]) == "Keyboard_Minimap");
    CHECK(cf[1].cmd == 0 && cf[1].in == Input{vk::Up} && std::string(kEmReserved[cf[1].reserved]) == "Keyboard_CameraOffset_Up");
    CHECK(!b.kb[Heal][0].valid() && b.pad[Heal][0] == (Input{0, 0, 1}) && !b.kb_move[0][1].valid() && b.kb_move[0][0] == Input{'W'});
    CHECK(conflict_bit(cf[0]) == (1u << Heal) && cf[1].move == 0 && conflict_bit(cf[1]) == (1u << 16)); // rows the Guest tints
    CHECK(b.kb[Interact][0] == Input{'E'} && b.pad[Interact][0] == (Input{0, xb::LThumb})); // EM's, not EL's F
    auto pressed = [&](std::initializer_list<int> keys, std::uint16_t buttons = 0, std::uint8_t lt = 0, std::uint8_t rt = 0) {
        Devices d;
        for (int k : keys) d.keys.set(k);
        if (buttons || lt || rt) d.pad = {true, buttons, lt, rt, 0, 0, 0, 0};
        return lily_actions(d, b).raw;
    };
    CHECK(pressed({}, xb::A) == (1u << SummonSwitch)); // the set switch has no Lilac counterpart: EL decides
    CHECK(pressed({}, 0, 0, 255) == (1u << Hook));     // one button, no chord
    CHECK(pressed({}, xb::RB) == (1u << Jump) && pressed({}, xb::Y) == (1u << Sub1) && pressed({}, xb::B) == (1u << Sub2));
    CHECK(pressed({}, xb::LThumb) == (1u << Interact) && pressed({}, 0, 255) == (1u << Heal));
    CHECK(pressed({'J', 'K', 'L', 'Q', 'P', vk::Space, vk::LShift}) ==
          ((1u << Attack) | (1u << Sub1) | (1u << Sub2) | (1u << SummonSwitch) | (1u << Hook) | (1u << Jump) | (1u << Dodge)));
    CHECK(pressed({'E'}) == (1u << Interact) && pressed({'F'}) == 0 && pressed({'R'}) == 0);
    CHECK(pressed({'Z', 'X', 'C', vk::Tab, vk::Escape, vk::Return, vk::Up, vk::Left}) == 0);
    Devices mv;
    mv.keys.set('D');
    CHECK(lily_actions(mv, b).x == 1.0f);
    mv.keys = Keys{};
    mv.keys.set(vk::Up); // EM's camera offset, not Lily's movement
    CHECK(lily_actions(mv, b).y == 0.0f);
    lie::ElBindings none; // nothing published yet: only the pad's stick / d-pad and EM's interact work
    compose_bindings(none, em, b);
    CHECK(pressed({vk::Space}) == 0 && pressed({'E'}) == (1u << Interact));

    // Lily's menus speak EM's UI keys
    const UiKeys ui = em_default_ui_keys();
    Keys m;
    for (int k : std::initializer_list<int>{vk::Return, vk::Escape, vk::Tab, 'E', 'Q', 'W', vk::Down}) m.set(k);
    const std::uint32_t mb = menu_bits(m, ui);
    for (int i : {lie::menu::Confirm, lie::menu::Cancel, lie::menu::Context, lie::menu::Next, lie::menu::Prev, lie::menu::Up, lie::menu::Down})
        CHECK(has(mb, i));
    CHECK(!has(mb, lie::menu::Left) && !has(mb, lie::menu::Right));
    Keys old_el; // EL's former menu keys no longer mean anything
    for (int k : std::initializer_list<int>{'Z', 'X', 'C', vk::Space, vk::LShift, 'H', 'J'}) old_el.set(k);
    CHECK(menu_bits(old_el, ui) == 0);
    UiKeys remapped = ui; // EM's UI remap moves the intent
    remapped.kb[lie::menu::Confirm][0] = {vk::Space};
    Keys space;
    space.set(vk::Space);
    CHECK(menu_bits(space, remapped) == (1u << lie::menu::Confirm) && menu_bits(space, ui) == 0);
    const Pad pad{true, static_cast<std::uint16_t>(xb::X | xb::RB | xb::Start | xb::Back), 255, 0, 0, 0, 0, 0};
    const lie::PadState ps = menu_pad(pad);
    CHECK(ps.connected && (ps.buttons & (xb::Start | xb::Back)) == 0 && (ps.buttons & xb::X) && ps.lt == 255);
    CHECK(menu_pad(Pad{}).connected == 0);
    CHECK(kElMenuVkDefault[lie::menu::Confirm] == vk::Return && kElMenuVkDefault[lie::menu::Context] == vk::Tab);
    for (int i = 0; i < lie::menu::Count; ++i) CHECK((kElUiType[i] == 0) == (kElMenuKbDefault[i][0] == 0));

    // Latch: what was held at the switch stays hidden until released, then works normally
    Latch l;
    Devices held;
    held.keys.set(vk::Escape);
    held.pad = {true, xb::B, 0, 200, 0, 20000, 0, 0};
    l.arm(held);
    Devices now = held;
    now.keys.set('E'); // pressed after the switch: passes
    l.apply(now);
    CHECK(!now.keys.down(vk::Escape) && now.keys.down('E') && l.blocks(vk::Escape));
    CHECK(now.pad.buttons == 0 && now.pad.rt == 0 && now.pad.ly == 0 && l.any());
    Devices released; // everything let go once
    released.pad.connected = true;
    l.apply(released);
    CHECK(!l.any() && !l.blocks(vk::Escape));
    Devices again = held; // pressed again: the new owner sees it
    l.apply(again);
    CHECK(again.keys.down(vk::Escape) && again.pad.buttons == xb::B && again.pad.rt == 200 && again.pad.ly == 20000);
    Latch partial; // releasing one input frees only that one
    partial.arm(held);
    Devices half = held;
    half.keys = Keys{};
    partial.apply(half);
    CHECK(!partial.blocks(vk::Escape) && half.pad.buttons == 0 && partial.any());
}

static void test_layout() {
    static_assert(sizeof(lie::Event) == 64);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(sizeof(lie::Control) < 64 * 1024 && sizeof(lie::World) < 256 * 1024);
    std::printf("[lie_tests] Control=%zu B World=%zu B\n", sizeof(lie::Control), sizeof(lie::World));
}

int main() {
    test_frame_map();
    test_seqlock_and_ring();
    test_pose_ring();
    test_terrain();
    test_combat();
    test_manifests();
    test_projection();
    test_input_routing();
    test_layout();
    std::printf("[lie_tests] %d checks passed\n", g_checks);
    return 0;
}
