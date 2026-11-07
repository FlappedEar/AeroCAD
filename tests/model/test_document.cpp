// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/model/Document.hpp"
#include "aerocad/model/UndoStack.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aerocad;
using model::Document;

namespace {
model::Part makePart(const char* name)
{
    core::TriangleMesh mesh;
    mesh.addVertex({0, 0, 0});
    mesh.addVertex({1, 0, 0});
    mesh.addVertex({0, 1, 0});
    mesh.addTriangle(0, 1, 2);
    model::Part p;
    p.name = name;
    p.mesh = std::make_shared<const core::TriangleMesh>(std::move(mesh));
    return p;
}
} // namespace

TEST_CASE("Document assigns ids and notifies observers", "[model]")
{
    Document doc;
    std::vector<std::pair<Document::Change, model::PartId>> events;
    const auto obs = doc.addObserver([&](Document::Change c, model::PartId id) {
        if (c != Document::Change::AboutToAdd && c != Document::Change::AboutToRemove)
            events.emplace_back(c, id);
    });

    const auto a = doc.addPart(makePart("a"));
    const auto b = doc.addPart(makePart("b"));
    CHECK(a != b);
    CHECK(doc.partCount() == 2);
    CHECK(doc.part(a)->name == "a");

    CHECK(doc.modifyPart(a, [](model::Part& p) { p.name = "renamed"; }));
    CHECK(doc.part(a)->revision == 1);
    CHECK_FALSE(doc.modifyPart(999, [](model::Part&) {}));

    auto removed = doc.removePart(b);
    REQUIRE(removed);
    CHECK(removed->name == "b");
    CHECK_FALSE(doc.removePart(b));

    REQUIRE(events.size() == 4);
    CHECK(events[0] == std::pair{Document::Change::Added, a});
    CHECK(events[2] == std::pair{Document::Change::Modified, a});
    CHECK(events[3] == std::pair{Document::Change::Removed, b});

    doc.removeObserver(obs);
    doc.addPart(makePart("c"));
    CHECK(events.size() == 4);
}

TEST_CASE("Document sends about-to notifications before changing the list", "[model]")
{
    Document doc;
    std::vector<std::size_t> countsAtNotification;
    doc.addObserver([&](Document::Change c, model::PartId) {
        if (c == Document::Change::AboutToAdd || c == Document::Change::AboutToRemove)
            countsAtNotification.push_back(doc.partCount());
    });
    const auto a = doc.addPart(makePart("a"));
    doc.removePart(a);
    CHECK(countsAtNotification == std::vector<std::size_t>{0, 1});
}

TEST_CASE("Document keeps explicit ids when re-adding", "[model]")
{
    Document doc;
    const auto a = doc.addPart(makePart("a"));
    auto removed = doc.removePart(a);
    CHECK(doc.addPart(*removed) == a);
    // A conflicting id gets a fresh one.
    auto copy = *doc.part(a);
    CHECK(doc.addPart(copy) != a);
}

TEST_CASE("Restoring an older snapshot still bumps the revision", "[model]")
{
    Document doc;
    const auto a = doc.addPart(makePart("a"));
    const model::Part snapshot = *doc.part(a); // revision 0
    doc.modifyPart(a, [](model::Part& p) { p.name = "b"; });
    CHECK(doc.part(a)->revision == 1);
    doc.modifyPart(a, [&](model::Part& p) { p = snapshot; });
    CHECK(doc.part(a)->revision == 2);
    CHECK(doc.part(a)->name == "a");
}

TEST_CASE("Document bounds skip hidden parts", "[model]")
{
    Document doc;
    const auto a = doc.addPart(makePart("a"));
    CHECK(doc.bounds().max.x == 1.0);
    doc.modifyPart(a, [](model::Part& p) { p.visible = false; });
    CHECK(doc.bounds().isEmpty());
}

TEST_CASE("UndoStack undo, redo and branch truncation", "[model]")
{
    model::UndoStack stack;
    int value = 0;
    auto set = [&](int from, int to) {
        value = to;
        stack.push({"set " + std::to_string(to), [&value, from] { value = from; }, [&value, to] { value = to; }});
    };
    CHECK_FALSE(stack.canUndo());
    set(0, 1);
    set(1, 2);
    CHECK(stack.undoLabel() == "set 2");
    CHECK(stack.undo());
    CHECK(value == 1);
    CHECK(stack.redoLabel() == "set 2");
    CHECK(stack.redo());
    CHECK(value == 2);
    CHECK(stack.undo());
    set(1, 5); // drops "set 2"
    CHECK_FALSE(stack.canRedo());
    CHECK(stack.size() == 2);
    CHECK(stack.undo());
    CHECK(stack.undo());
    CHECK(value == 0);
    CHECK_FALSE(stack.undo());
}

TEST_CASE("UndoStack respects its limit", "[model]")
{
    model::UndoStack stack(3);
    for (int i = 0; i < 5; ++i)
        stack.push({std::to_string(i), [] {}, [] {}});
    CHECK(stack.size() == 3);
    CHECK(stack.undoLabel() == "4");
}

TEST_CASE("Document revision increases on every change", "[model]")
{
    Document doc;
    CHECK(doc.revision() == 0);
    const auto a = doc.addPart(makePart("a"));
    CHECK(doc.revision() == 1);
    doc.modifyPart(a, [](model::Part& p) { p.name = "b"; });
    CHECK(doc.revision() == 2);
    CHECK_FALSE(doc.modifyPart(999, [](model::Part&) {}));
    CHECK(doc.revision() == 2);
    doc.removePart(a);
    CHECK(doc.revision() == 3);
}

TEST_CASE("Only imported geometry changes invalidate source dependencies", "[model][source]")
{
    Document doc;
    CHECK(doc.importedGeometryRevision() == 0);
    const auto source = doc.addPart(makePart("source"));
    const auto original = *doc.part(source);
    CHECK(doc.importedGeometryRevision() == 1);
    doc.modifyPart(source, [](model::Part& p) {
        p.name = "renamed";
        p.visible = false;
        p.selection = std::make_shared<const core::TriangleSet>(core::TriangleSet{0});
    });
    CHECK(doc.importedGeometryRevision() == 1);
    doc.modifyPart(source, [](model::Part& p) { p.mesh = makePart("replacement").mesh; });
    CHECK(doc.importedGeometryRevision() == 2);
    doc.modifyPart(source, [&](model::Part& p) { p = original; });
    CHECK(doc.importedGeometryRevision() == 3);

    auto body = makePart("body");
    body.kind = model::PartKind::Body;
    const auto derived = doc.addPart(body);
    doc.modifyPart(derived, [](model::Part& p) { p.mesh = makePart("derived update").mesh; });
    doc.removePart(derived);
    CHECK(doc.importedGeometryRevision() == 3);
    auto removed = doc.removePart(source);
    CHECK(doc.importedGeometryRevision() == 4);
    doc.addPart(*removed);
    CHECK(doc.importedGeometryRevision() == 5);
    doc.modifyPart(source, [](model::Part& p) { p.kind = model::PartKind::Body; });
    CHECK(doc.importedGeometryRevision() == 6);
}

TEST_CASE("A throwing imported geometry edit preserves handles and revisions", "[model][source]")
{
    Document doc;
    auto part = makePart("source");
    part.shape = geometry::makeBox(1, 2, 3);
    const auto source = doc.addPart(part);
    const auto original = *doc.part(source);
    const auto documentRevision = doc.revision();
    const auto sourceRevision = doc.importedGeometryRevision();
    int notifications = 0;
    doc.addObserver([&](Document::Change, model::PartId) { ++notifications; });

    CHECK_THROWS_AS(doc.modifyPart(source, [](model::Part& updated) {
        updated.mesh = makePart("replacement").mesh;
        updated.shape = geometry::makeBox(4, 5, 6);
        updated.kind = model::PartKind::Body;
        updated.name = "partial update";
        throw std::runtime_error("geometry edit failed");
    }), std::runtime_error);

    REQUIRE(doc.part(source));
    CHECK(doc.part(source)->id == original.id);
    CHECK(doc.part(source)->name == original.name);
    CHECK(doc.part(source)->kind == original.kind);
    CHECK(doc.part(source)->mesh == original.mesh);
    REQUIRE(doc.part(source)->shape);
    CHECK(doc.part(source)->shape->impl() == original.shape->impl());
    CHECK(doc.part(source)->revision == original.revision);
    CHECK(doc.revision() == documentRevision);
    CHECK(doc.importedGeometryRevision() == sourceRevision);
    CHECK(notifications == 0);
}

TEST_CASE("UndoStack joins the last steps into one", "[model]")
{
    model::UndoStack stack;
    std::vector<int> log;
    auto step = [&](int n) {
        stack.push({"step " + std::to_string(n), [&log, n]() { log.push_back(-n); }, [&log, n]() { log.push_back(n); }});
    };
    step(1);
    step(2);
    step(3);
    CHECK_FALSE(stack.mergeLast(4, "too many"));
    CHECK_FALSE(stack.mergeLast(1, "one"));
    REQUIRE(stack.mergeLast(2, "Fillet"));
    CHECK(stack.size() == 2);
    CHECK(stack.undoLabel() == "Fillet");
    REQUIRE(stack.undo());
    CHECK(log == std::vector<int>{-3, -2});
    REQUIRE(stack.redo());
    CHECK(log == std::vector<int>{-3, -2, 2, 3});
    REQUIRE(stack.undo());
    REQUIRE(stack.undo());
    CHECK(stack.undoLabel().empty());
}

TEST_CASE("CAD-86: UndoStack tokens identify a caller's own steps", "[model]")
{
    model::UndoStack stack;
    auto step = [&](const std::string& label) { stack.push({label, [] {}, [] {}}); };
    CHECK(stack.topId() == 0);
    step("convert");
    const std::uint64_t convert = stack.topId();
    step("rename by MCP");
    const std::uint64_t rename = stack.topId();
    CHECK(convert != 0);
    CHECK(rename != convert);
    CHECK(stack.isApplied(convert));
    CHECK(stack.topIds(5) == std::vector<std::uint64_t>{convert, rename});

    // Someone else's step is in between: the dialog's steps are not joined.
    step("combine");
    const std::uint64_t combine = stack.topId();
    CHECK_FALSE(stack.mergeIds({convert, combine}, "Combine"));
    CHECK(stack.size() == 3);
    // Exactly the last steps: joined, under a new token.
    REQUIRE(stack.mergeIds({rename, combine}, "joined"));
    CHECK(stack.size() == 2);
    CHECK(stack.topId() != combine);
    CHECK_FALSE(stack.isApplied(combine));

    // Undone steps are no longer applied; tokens are never reused, also after clear().
    REQUIRE(stack.undo());
    CHECK(stack.topId() == convert);
    stack.clear();
    CHECK_FALSE(stack.isApplied(convert));
    step("after clear");
    CHECK(stack.topId() > convert);
    CHECK(stack.topId() != rename);
}
