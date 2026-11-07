// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/model/Document.hpp"
#include "aerocad/model/UndoStack.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
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
