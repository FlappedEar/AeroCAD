// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/app/PickFilter.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace aerocad;

TEST_CASE("Viewport picking follows displayed visibility", "[app][picking]")
{
    app::PickFilter filter;
    model::Part part;
    part.id = 7;
    part.featureId = 12;
    part.kind = model::PartKind::Sketch;
    CHECK(filter.accepts(part));
    part.visible = false;
    CHECK_FALSE(filter.accepts(part));
    filter.shownSketch = 11;
    CHECK_FALSE(filter.accepts(part));
    filter.shownSketch = 12;
    CHECK(filter.accepts(part));
    filter.hiddenPartIds = {7};
    CHECK_FALSE(filter.accepts(part));
    part.visible = true;
    CHECK_FALSE(filter.accepts(part));
    filter.hiddenPartIds = {8};
    CHECK(filter.accepts(part));
    part.visible = false;
    for (const auto kind : {model::PartKind::Body, model::PartKind::Imported, model::PartKind::Plane}) {
        part.kind = kind;
        CHECK_FALSE(filter.accepts(part));
    }
    part.kind = model::PartKind::Sketch;
    part.featureId = 0;
    filter.shownSketch.reset();
    CHECK_FALSE(filter.accepts(part));
}

TEST_CASE("Viewport pick kinds exclude occluding preview bodies", "[app][picking]")
{
    app::PickFilter filter;
    const std::vector<std::pair<std::string, model::PartKind>> kinds{
        {"body", model::PartKind::Body}, {"sketch", model::PartKind::Sketch},
        {"imported", model::PartKind::Imported}, {"plane", model::PartKind::Plane}};
    model::Part part;
    for (const auto& [name, kind] : kinds) {
        part.kind = kind;
        filter.kind.clear();
        CHECK(filter.accepts(part));
        for (const auto& [requested, requestedKind] : kinds) {
            filter.kind = requested;
            CHECK(filter.accepts(part) == (kind == requestedKind));
        }
    }
    filter.kind = "solid";
    part.kind = model::PartKind::Body;
    CHECK(filter.accepts(part));
    part.kind = model::PartKind::Imported;
    CHECK_FALSE(filter.accepts(part));
    // Matches the renderer's hasBrep role: imported exact surfaces also qualify.
    part.shape.emplace();
    CHECK(filter.accepts(part));
    part.kind = model::PartKind::Sketch;
    CHECK_FALSE(filter.accepts(part));
    part.kind = model::PartKind::Plane;
    CHECK_FALSE(filter.accepts(part));
    filter.kind = "unknown";
    CHECK_FALSE(filter.accepts(part));
}

TEST_CASE("Viewport picking excludes only the dialog's own construction plane", "[app][picking]")
{
    app::PickFilter filter;
    model::Part part;
    part.kind = model::PartKind::Plane;
    part.featureId = 15;
    CHECK(filter.accepts(part));
    filter.unpickablePlane = 15;
    CHECK_FALSE(filter.accepts(part));
    part.featureId = 16;
    CHECK(filter.accepts(part));
    part.featureId = 15;
    part.kind = model::PartKind::Body;
    CHECK(filter.accepts(part));
    part.kind = model::PartKind::Sketch;
    CHECK(filter.accepts(part));
}
