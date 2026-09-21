#!/usr/bin/env python3
"""Check source-commit release metadata and the actual OBS title method with Qt."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "frontend/widgets/OBSBasic.cpp").read_text()
method = "void OBSBasic::UpdateTitleBar()" + source.split("void OBSBasic::UpdateTitleBar()", 1)[1].split("OBSBasic *OBSBasic::Get()", 1)[0]
copy = (root / "frontend/data/locale/en-US.ini").read_text().split('TitleBar.UpdateReleasedOn="', 1)[1].split('"', 1)[0]
qt = next((root / ".deps").glob("obs-deps-qt6-*/lib/QtCore.framework")).parent

fixture = r'''
#include <QCoreApplication>
#include <QDate>
#include <QLocale>
#include <QString>
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
using namespace std;
#define OBS_PLUS_PLUS_RELEASE_DATE "2026-09-21"
#define QT_UTF8(value) QString::fromUtf8(value)
#define QT_TO_UTF8(value) (value).toUtf8().constData()
bool safe_mode = false;
string profile = "Coffee & coding", collection = "Scènes";
const char *config_get_string(void *, const char *, const char *key) {
    return string(key) == "Profile" ? profile.c_str() : collection.c_str();
}
const char *Str(const char *key) {
    if (string(key) == "TitleBar.Profile") return "Profile";
    if (string(key) == "TitleBar.Scenes") return "Scenes";
    if (string(key) == "TitleBar.SafeMode") return "SAFE MODE";
    return "Portable Mode";
}
QString QTStr(const char *) { return QStringLiteral(RELEASE_COPY); }
struct TestApp {
    bool portable = false;
    void *GetUserConfig() { return nullptr; }
    string GetVersionString(bool) { return "32.2.2-obs-plus-plus"; }
    bool IsPortableMode() { return portable; }
} app;
TestApp *App() { return &app; }
struct OBSBasic {
    bool previewProgramMode = false;
    QString title;
    void setWindowTitle(QString value) { title = value; }
    void UpdateTitleBar();
};
'''.replace("RELEASE_COPY", '"' + copy + '"')
fixture += method
fixture += r'''
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QLocale::setDefault(QLocale::French);
    OBSBasic window;
    for (int flags = 0; flags < 8; ++flags) {
        window.previewProgramMode = flags & 1;
        safe_mode = flags & 2;
        app.portable = flags & 4;
        window.UpdateTitleBar();
        QString expected = "OBS++ ";
        if (flags & 1) expected += "Studio ";
        expected += "32.2.2-obs-plus-plus";
        if (flags & 2) expected += " (SAFE MODE)";
        if (flags & 4) expected += " - Portable Mode";
        expected += " - Profile: Coffee & coding - Scenes: Scènes - Update released on 21 September 2026";
        assert(window.title == expected);
    }
    profile = "3000AD Music";
    collection = "3000ad v3 new";
    window.UpdateTitleBar();
    assert(window.title.endsWith(" - Profile: 3000AD Music - Scenes: 3000ad v3 new - Update released on 21 September 2026"));
    assert(window.title.count("Update released on") == 1);
    std::cout << "PASS: 8 title modes, profile/collection update, human-readable fixed release date\n";
}
'''

with tempfile.TemporaryDirectory(prefix="obs-release-title-") as temporary:
    directory = Path(temporary)
    (directory / "title.cpp").write_text(fixture)
    subprocess.run(["c++", "-std=c++17", "-fPIC", "-F" + str(qt), "-I" + str(qt / "QtCore.framework/Headers"),
                    "-framework", "QtCore", "-Wl,-rpath," + str(qt), str(directory / "title.cpp"),
                    "-o", str(directory / "title")], check=True)
    subprocess.run([str(directory / "title")], check=True)
    subprocess.run(["git", "init", "-q", str(directory)], check=True)
    environment = os.environ | {"GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "test@example.invalid",
                                "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "test@example.invalid"}
    (directory / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.28)\nproject(release_test NONE)\n'
        f'include("{root / "frontend/cmake/obs-plus-plus-release.cmake"}")\n'
        'configure_file(date.in date.txt @ONLY)\n')
    (directory / "date.in").write_text("@OBS_PLUS_PLUS_RELEASE_DATE@\n")
    for date in ("2025-12-31", "2026-09-21"):
        environment |= {"GIT_AUTHOR_DATE": date + "T12:00:00+0000", "GIT_COMMITTER_DATE": date + "T12:00:00+0000"}
        subprocess.run(["git", "-C", str(directory), "commit", "-q", "--allow-empty", "-m", "release fixture"],
                       env=environment, check=True)
        command = (["cmake", "-S", str(directory), "-B", str(directory / "build"), "-G", "Unix Makefiles"]
                   if date == "2025-12-31" else ["cmake", "--build", str(directory / "build")])
        subprocess.run(command, check=True, stdout=subprocess.PIPE)
        assert (directory / "build/date.txt").read_text().strip() == date
    print("PASS: release date follows the source commit and refreshes on incremental build")
