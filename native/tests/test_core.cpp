// Offline contract tests for the engine-independent core (S0 gate: `build.bat core test`).

#include "core/binary_manifest.h"
#include "core/combat_map.h"
#include "core/frame_map.h"
#include "core/pose_ring.h"
#include "core/projection.h"
#include "core/protocol.h"
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
    test_layout();
    std::printf("[lie_tests] %d checks passed\n", g_checks);
    return 0;
}
