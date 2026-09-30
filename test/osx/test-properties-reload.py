#!/usr/bin/env python3
"""Exercise both real property-update callbacks with Qt's event queue, without OBS."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
qt = next((root / ".deps").glob("obs-deps-qt6-*/lib/QtCore.framework")).parent
callbacks = []
for name in ("OBSBasicProperties", "OBSBasicFilters"):
    source = (root / "frontend/dialogs" / (name + ".cpp")).read_text()
    signature = "void " + name + "::UpdateProperties(void *data, calldata_t *)"
    callbacks.append(signature + source.split(signature, 1)[1].split("\n}\n", 1)[0] + "\n}\n")

fixture = r'''
#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QObject>
#include <cassert>
#include <cstdio>
struct calldata_t {};
struct OBSPropertiesView : QObject {
    bool button_handler_running = false;
    int reloads = 0;
    void ReloadProperties() { assert(!button_handler_running); reloads++; }
};
struct OBSBasicProperties {
    OBSPropertiesView *view;
    static void UpdateProperties(void *, calldata_t *);
};
struct OBSBasicFilters {
    OBSPropertiesView *view;
    static void UpdateProperties(void *, calldata_t *);
};
'''
fixture += "\n".join(callbacks)
fixture += r'''
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    OBSPropertiesView view;
    OBSBasicProperties properties{&view};
    OBSBasicFilters filters{&view};
    view.button_handler_running = true;
    OBSBasicProperties::UpdateProperties(&properties, nullptr);
    OBSBasicFilters::UpdateProperties(&filters, nullptr);
    assert(view.reloads == 0);
    view.button_handler_running = false;
    QCoreApplication::sendPostedEvents(&view, QEvent::MetaCall);
    assert(view.reloads == 2);
    auto *closed_view = new OBSPropertiesView;
    OBSBasicProperties closed{closed_view};
    OBSBasicProperties::UpdateProperties(&closed, nullptr);
    delete closed_view;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    puts("PASS: both Properties and Filters reload after the active handler, and closing cancels a pending reload");
}
'''

with tempfile.TemporaryDirectory(prefix="obs-properties-test-") as directory:
    harness = Path(directory) / "properties.cpp"
    binary = Path(directory) / "properties"
    harness.write_text(fixture)
    subprocess.run(["xcrun", "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fPIC", "-F" + str(qt), "-I" + str(qt / "QtCore.framework/Headers"),
                    "-framework", "QtCore", "-Wl,-rpath," + str(qt), str(harness), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
