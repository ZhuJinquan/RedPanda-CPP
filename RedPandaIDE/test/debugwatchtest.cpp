#include <QtTest>
#include "debugger/debugger.h"
#include "debugger/gdbmidebugger.h"
#include "settings/debuggersettings.h"
#include "compiler/compiler.h"
#include "mainwindow.h"
#include "editormanager.h"
#include "editor.h"
#include "systemconsts.h"
#include "autolinkmanager.h"
#include "utils/os.h"
#include "cpprefacter.h"
#include "project.h"
#include <qt_utils/charsetinfo.h>
#include <qsynedit/syntaxer/cpp.h>
#include <qsynedit/formatter/cppformatter.h>
#include <QClipboard>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>

class UiRegressionContext {
public:
    QTemporaryDir directory;
    SystemConsts systemConsts;
    CharsetInfoManager charset{"en_US"};
    Settings settings{directory.filePath("settings.ini")};
    AutolinkManager autolinks;
    ExternalResource resources;
    std::unique_ptr<MainWindow> window;

    UiRegressionContext() {
        pSystemConsts = &systemConsts;
        pCharsetInfoManager = &charset;
        pSettings = &settings;
        pAutolinkManager = &autolinks;
        initParser();
        auto compiler = std::make_shared<CompilerSet>();
        compiler->setName("Regression compiler");
        compiler->setCompilerType(CompilerType::GCC);
        settings.compilerSets().addSet(compiler);
        settings.compilerSets().setDefaultIndex(0);
        settings.compilerSets().saveSets();
        settings.sync();
        settings.load();
        settings.codeCompletion().setEnabled(false);
        window = std::make_unique<MainWindow>();
        pMainWindow = window.get();
    }
    ~UiRegressionContext() {
        window.reset();
        pMainWindow = nullptr;
        pAutolinkManager = nullptr;
        pSettings = nullptr;
        pCharsetInfoManager = nullptr;
        pSystemConsts = nullptr;
    }
};

class CompilerArgumentProbe : public Compiler {
public:
    PCompilerSet set = std::make_shared<CompilerSet>();
    CompilerArgumentProbe() : Compiler("control.c", false) {
        set->setCompilerType(CompilerType::GCC);
    }
    using Compiler::getCCompileArguments;
    using Compiler::getCppCompileArguments;
    using Compiler::getLibraryArguments;
    using Compiler::runCommand;
protected:
    PCompilerSet compilerSet() override { return set; }
    bool prepareForCompile() override { return false; }
    bool prepareForRebuild() override { return false; }
};

class ReferenceUrlCapture : public QObject {
    Q_OBJECT
public:
    QUrl lastUrl;
public slots:
    void capture(const QUrl& url) { lastUrl = url; }
};

class TestClient : public GDBMIDebuggerClient {
public:
    explicit TestClient(Debugger *debugger)
        : GDBMIDebuggerClient(debugger, DebuggerType::GDB, debugger) {
        mInferiorRunning = false;
    }
    void setRunning(bool running) { mInferiorRunning = running; }
};

class DebugWatchTest : public QObject {
    Q_OBJECT
    static TestClient *attach(Debugger &debugger) {
        auto *client = new TestClient(&debugger);
        debugger.mClient = client;
        return client;
    }
private slots:
    void projectSymbolReferences_data() {
        QTest::addColumn<QString>("projectFile");
        QTest::newRow("extern-variable") << QString();
        const QString project = qEnvironmentVariable("REDPANDA_REFERENCE_PROJECT");
        if (!project.isEmpty())
            QTest::newRow("user-project") << project;
    }
    void projectSymbolReferences() {
        QFETCH(QString, projectFile);
        UiRegressionContext ui;
        const QString copy = ui.directory.filePath("references.dev");
        SimpleIni settings;
        QStringList files;
        if (projectFile.isEmpty()) {
            for (const auto &entry : QList<QPair<QString,QByteArray>>{
                     {"shared.h", "extern float M_SPD_fout;\n"},
                     {"define.c", "#include \"shared.h\"\nfloat M_SPD_fout;\n"},
                     {"use.c", "#include \"shared.h\"\nvoid update(void) { M_SPD_fout = 1; }\n"}}) {
                const QString path = ui.directory.filePath(entry.first);
                QFile file(path);
                QVERIFY(file.open(QIODevice::WriteOnly));
                QCOMPARE(file.write(entry.second), qint64(entry.second.size()));
                files.append(path);
            }
        } else {
            SimpleIni original;
            QVERIFY(original.LoadFile(projectFile.toLocal8Bit()) >= 0);
            const QDir base(QFileInfo(projectFile).absolutePath());
            const int count = original.GetLongValue("Project", "UnitCount", 0);
            QVERIFY(count > 0);
            for (int i=1; i<=count; ++i)
                files.append(QDir::cleanPath(base.absoluteFilePath(QString::fromUtf8(original.GetValue(QString("Unit%1").arg(i).toUtf8(), "FileName", "")))));
            QStringList includes;
            for (const QString &path : QString::fromUtf8(original.GetValue("Project", "Includes", "")).split(';', Qt::SkipEmptyParts))
                includes.append(QDir::cleanPath(base.absoluteFilePath(path)));
            settings.SetValue("Project", "Includes", includes.join(';').toUtf8());
        }
        settings.SetValue("Project", "Name", "Reference regression");
        settings.SetLongValue("Project", "Ver", 3);
        settings.SetLongValue("Project", "Type", 1);
        settings.SetLongValue("Project", "UnitCount", files.size());
        for (int i=0; i<files.size(); ++i) {
            const QByteArray section = QString("Unit%1").arg(i+1).toUtf8();
            settings.SetValue(section, "FileName", files[i].toUtf8());
            settings.SetValue(section, "FileEncoding", "UTF-8");
        }
        QVERIFY(settings.SaveFile(copy.toLocal8Bit()) >= 0);
        ui.window->openProject(copy, false);
        QVERIFY(ui.window->project());
        auto parser = ui.window->project()->cppParser();
        parser->setEnabled(true);
        parser->addHardDefineByLine("#define HOST_SIM 1");
        ui.window->project()->resetParserProjectFiles();
        CppParser::parseFileListNonBlocking(parser, false);
        QTRY_VERIFY_WITH_TIMEOUT(!parser->parsing() && parser->findStatement("M_SPD_fout"), 30000);
        const QString symbol = "M_SPD_fout";
        int searched = 0;
        for (const QString &path : files) {
            auto editor = ui.window->editorManager()->newEditor(path, ENCODING_UTF8, getFileType(path), "", true, false);
            QVERIFY(editor);
            editor->setCppParser(parser);
            int row = -1;
            int column = -1;
            QSynedit::CppSyntaxer syntaxer;
            for (int i=0; i<editor->lineCount() && row<0; ++i) {
                editor->startParseLine(&syntaxer, i);
                while (!syntaxer.eol()) {
                    if (syntaxer.getToken() == symbol
                            && syntaxer.getTokenAttribute()->tokenType() == QSynedit::TokenType::Identifier) {
                        row = i;
                        column = syntaxer.getTokenPos();
                        break;
                    }
                    syntaxer.next();
                }
            }
            if (row < 0)
                continue; // No occurrence outside comments/strings in this file.
            const QSynedit::CharPos pos{column+2, row};
            QVERIFY2(parser->findStatementOf(path, editor->getExpressionAtPosition(pos), row), qPrintable(path));
            CppRefacter refactor(ui.window.get());
            QVERIFY2(refactor.findOccurence(editor, pos), qPrintable(path));
            const auto results = ui.window->searchResultModel()->currentResults();
            QVERIFY(results);
            qInfo() << "Reference probe" << path << "files:" << results->results().size();
            QVERIFY2(!results->results().isEmpty(), qPrintable(path));
            QVERIFY2(results->results().size() >= 3, qPrintable(path));
            const QSynedit::CharPos end{column+symbol.size(), row};
            editor->setCaretAndSelection(end, QSynedit::CharPos{end.ch-symbol.size(), row}, end);
            QVERIFY(QMetaObject::invokeMethod(ui.window.get(), "on_actionFind_references_triggered", Qt::DirectConnection));
            // The action must create a fresh result rather than leave the
            // previous history entry on screen after silently failing.
            QVERIFY2(ui.window->searchResultModel()->currentResults() != results, qPrintable(path));
            QCOMPARE(ui.window->searchResultModel()->currentResults()->keyword(), symbol);
            QCOMPARE(ui.window->searchResultModel()->currentResults()->results().size(), results->results().size());
            ++searched;
        }
        QVERIFY(searched >= 3);
    }
    void symbolReferencesAtSelectionEnd_data() {
        QTest::addColumn<QString>("symbol");
        QTest::addColumn<int>("offset");
        QTest::newRow("variable-inside") << "counter" << 2;
        QTest::newRow("variable-selection-end") << "counter" << 7;
        QTest::newRow("function-inside") << "tick" << 2;
        QTest::newRow("function-selection-end") << "tick" << 4;
    }
    void symbolReferencesAtSelectionEnd() {
        QFETCH(QString, symbol);
        QFETCH(int, offset);
        UiRegressionContext ui;
        const QString filename = ui.directory.filePath("references.cpp");
        const QByteArray source = "int counter;\nvoid tick() { ++counter; }\nvoid run() { tick(); counter=2; }\n";
        QFile file(filename);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(source), qint64(source.size()));
        file.close();
        Editor editor(nullptr);
        editor.setEditorSettings(&ui.settings.editor());
        editor.setFilename(filename);
        editor.setFileType(FileType::CppSource);
        editor.setContent(QString::fromUtf8(source));
        auto parser = std::make_shared<CppParser>();
        editor.setCppParser(parser);
        CppParser::parseFileBlocking(parser, filename, false, "", false, false);
        const int start = editor.lineText(2).indexOf(symbol);
        QVERIFY(start >= 0);
        CppRefacter refactor(ui.window.get());
        QVERIFY(refactor.findOccurence(&editor, QSynedit::CharPos{start+offset, 2}));
        auto results = ui.window->searchResultModel()->currentResults();
        QVERIFY(results);
        QCOMPARE(results->keyword(), symbol);
        QCOMPARE(results->results().size(), 1);
        const int expected = symbol == "counter" ? 3 : 2;
        QCOMPARE(results->results()[0]->results.size(), expected);
    }
#ifdef Q_OS_WIN
    void bundledToolsEnvironmentIsOptional() {
        QTemporaryDir directory;
        QProcessEnvironment original;
        original.insert("PATH", "original-bin;second-bin");
        original.insert("SHELL", "existing-shell");
        original.insert("CMAKE_GENERATOR", "MinGW Makefiles");
        QVERIFY(bundledBuildToolsEnvironment(original, directory.path()) == original);
    }
    void bundledToolsDiscoverAfterRelocation() {
        QTemporaryDir directory;
        const QString root = directory.filePath(QString::fromUtf8("IDE 中文 with spaces"));
        QVERIFY(QDir().mkpath(root+"/tools/cmake/bin"));
        QVERIFY(QDir().mkpath(root+"/tools/cmake/share"));
        QVERIFY(QDir().mkpath(root+"/tools/ninja"));
        for (const QString& name : {QString("tools/cmake/bin/cmake.exe"), QString("tools/ninja/ninja.exe")}) {
            QFile file(root+"/"+name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("fixture");
        }
        QProcessEnvironment original;
        original.insert("PATH", "original-bin;second-bin");
        original.insert("CMAKE_GENERATOR", "MinGW Makefiles");
        original.insert("CMAKE_MAKE_PROGRAM", "original-mingw32-make.exe");
        const auto configured = bundledBuildToolsEnvironment(original, root);
        QCOMPARE(QDir::fromNativeSeparators(configured.value("CMAKE_HOME")), root+"/tools/cmake");
        const auto path = configured.value("PATH").split(';');
        QCOMPARE(QDir::fromNativeSeparators(path[0]), root+"/tools/cmake/bin");
        QCOMPARE(QDir::fromNativeSeparators(path[1]), root+"/tools/ninja");
        QCOMPARE(path.mid(2), QStringList({"original-bin","second-bin"}));
        QCOMPARE(configured.value("CMAKE_GENERATOR"), original.value("CMAKE_GENERATOR"));
        QCOMPARE(configured.value("CMAKE_MAKE_PROGRAM"), original.value("CMAKE_MAKE_PROGRAM"));
        QVERIFY(bundledBuildToolsEnvironment(configured, root) == configured);
        QVERIFY(!original.contains("CMAKE_HOME"));
        const QString moved = directory.filePath("Moved IDE");
        QVERIFY(QDir().rename(root, moved));
        const auto relocated = bundledBuildToolsEnvironment(original, moved);
        QCOMPARE(QDir::fromNativeSeparators(relocated.value("CMAKE_HOME")), moved+"/tools/cmake");
        QVERIFY(!relocated.value("PATH").contains(root));
    }
    void explicitCMakeHomeIsPreserved() {
        QTemporaryDir directory;
        const QString root = directory.filePath("IDE");
        const QString custom = directory.filePath("Custom CMake");
        for (const QString& path : {root+"/tools/cmake/bin", root+"/tools/cmake/share", custom+"/bin"})
            QVERIFY(QDir().mkpath(path));
        for (const QString& name : {root+"/tools/cmake/bin/cmake.exe", custom+"/bin/cmake.exe"}) {
            QFile file(name);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QProcessEnvironment original;
        original.insert("PATH", "original-bin");
        original.insert("CMAKE_HOME", custom);
        const auto configured = bundledBuildToolsEnvironment(original, root);
        QCOMPARE(configured.value("CMAKE_HOME"), custom);
        QCOMPARE(QDir::fromNativeSeparators(configured.value("PATH").split(';')[0]), custom+"/bin");
        QVERIFY(!configured.value("PATH").contains(root));
        original.insert("CMAKE_HOME", directory.filePath("missing-explicit-choice"));
        QVERIFY(bundledBuildToolsEnvironment(original, root) == original);
    }
    void incompleteCMakePackageIsNotSelected() {
        QTemporaryDir directory;
        QVERIFY(QDir().mkpath(directory.path()+"/tools/cmake/bin"));
        QFile file(directory.path()+"/tools/cmake/bin/cmake.exe");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QProcessEnvironment original;
        original.insert("PATH", "original-bin");
        QVERIFY(bundledBuildToolsEnvironment(original, directory.path()) == original);
    }
    void legacyMakeUsesBundledCMakeAndNinja() {
        const QString make = qEnvironmentVariable("REDPANDA_TEST_MAKE");
        if (make.isEmpty())
            QSKIP("Set REDPANDA_TEST_MAKE and stage bundled tools to run the real Make/CMake/Ninja integration.");
        QVERIFY(QFileInfo(make).isFile());
        const QString toolRoot = QCoreApplication::applicationDirPath()+"/tools";
        QFile manifestFile(toolRoot+"/build-tools-manifest.json");
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        UiRegressionContext ui;
        QTemporaryDir project;
        const QString gcc = QDir::fromNativeSeparators(QFileInfo(make).absolutePath()+"/gcc.exe");
        const auto write = [&](const QString& name, const QByteArray& data) {
            QFile file(project.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(write("main.c", "int main(void) { return 0; }\n"));
        QVERIFY(write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.19)\nproject(legacy_probe LANGUAGES C)\nadd_executable(probe main.c)\n"));
        const QByteArray recipe = "all:\n\t@cmake --version > cmake-version.txt\n\t@ninja --version > ninja-version.txt\n\t@cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=\""
                + gcc.toUtf8() + "\"\n\t@cmake --build build\n\t@build\\probe.exe\n\t@echo success> success.txt\n";
        QVERIFY(write("Makefile", recipe));
        CompilerArgumentProbe compiler;
        compiler.set->setCCompiler(gcc);
        compiler.set->binDirs().append(QFileInfo(make).absolutePath());
        QSignalSpy errors(&compiler, &Compiler::compileErrorOccured);
        QSignalSpy output(&compiler, &Compiler::compileOutput);
        compiler.runCommand(make, {"-f","Makefile","all"}, project.path());
        QString transcript;
        for (const auto &arguments : output)
            transcript += arguments[0].toString();
        QVERIFY2(errors.isEmpty(), qPrintable(transcript));
        QVERIFY2(QFileInfo(project.filePath("success.txt")).isFile(), qPrintable(transcript));
        QFile cmakeVersion(project.filePath("cmake-version.txt"));
        QVERIFY(cmakeVersion.open(QIODevice::ReadOnly));
        QVERIFY(cmakeVersion.readAll().contains(manifest["cmake"].toObject()["version"].toString().toUtf8()));
        QFile ninjaVersion(project.filePath("ninja-version.txt"));
        QVERIFY(ninjaVersion.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(ninjaVersion.readAll()).trimmed(), manifest["ninja"].toObject()["version"].toString());
        QFile cache(project.filePath("build/CMakeCache.txt"));
        QVERIFY(cache.open(QIODevice::ReadOnly));
        const QByteArray contents = cache.readAll();
        QVERIFY(contents.contains("CMAKE_GENERATOR:INTERNAL=Ninja"));
        QVERIFY(contents.contains(QDir::fromNativeSeparators(toolRoot+"/ninja/ninja.exe").toUtf8()));
    }
#endif
    void pauseActivatesSourceEditor() {
        UiRegressionContext ui;
        const QString source = ui.directory.filePath("control.c");
        const QString other = ui.directory.filePath("other.c");
        for (const QString& filename : {source, other}) {
            QFile file(filename);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("int a;\nint b;\nint c;\n");
        }
        Editor *sourceEditor = ui.window->openFile(source);
        QVERIFY(sourceEditor);
        Editor *otherEditor = ui.window->openFile(other);
        QVERIFY(otherEditor);
        QCOMPARE(ui.window->editorManager()->getEditor(), otherEditor);
        Debugger debugger;
        auto *client = attach(debugger);
        connect(client, &DebuggerClient::inferiorStopped,
                ui.window.get(), &MainWindow::setActiveBreakpoint);
        client->setRunning(true);
        const QByteArray record = "*stopped,reason=\"signal-received\",signal-name=\"SIGINT\",frame={fullname=\""
                + source.toUtf8() + "\",line=\"3\"}";
        client->processExecAsyncRecord(record);
        QCOMPARE(ui.window->editorManager()->getEditor(), sourceEditor);
        QCOMPARE(sourceEditor->caretXY().line, 2);
        QCOMPARE(sourceEditor->caretXY().ch, 0);
        QVERIFY(!client->inferiorRunning());
        QVERIFY(!client->mResumeAfterBreakpointEdit);
    }
    void singleLinePastePreservesWhitespace_data() {
        QTest::addColumn<QString>("before");
        QTest::addColumn<int>("position");
        QTest::addColumn<QString>("clipboard");
        QTest::addColumn<QString>("expected");
        QTest::newRow("issue-766") << "    b;" << 4 << "a += " << "    a += b;";
        QTest::newRow("spaces-only") << "b;" << 0 << "   " << "   b;";
        QTest::newRow("tabs-and-spaces") << "\tb;" << 1 << " \t " << "\t \t b;";
        QTest::newRow("leading-and-trailing") << "b;" << 0 << " a += " << " a += b;";
        QTest::newRow("inside-expression") << "a+b;" << 2 << "  c + " << "a+  c + b;";
    }
    void singleLinePastePreservesWhitespace() {
        QFETCH(QString, before);
        QFETCH(int, position);
        QFETCH(QString, clipboard);
        QFETCH(QString, expected);
        QSynedit::QSynEdit editor;
        editor.setSyntaxer(std::make_shared<QSynedit::CppSyntaxer>());
        editor.setFormatter(std::make_shared<QSynedit::CppFormatter>());
        auto options = editor.getOptions();
        options.setFlag(QSynedit::EditorOption::AutoIndent, true);
        options.setFlag(QSynedit::EditorOption::GroupUndo, false);
        editor.setOptions(options);
        editor.setContent(before);
        const QSynedit::CharPos pos{position,0};
        editor.setCaretAndSelection(pos,pos,pos);
        QApplication::clipboard()->setText(clipboard);
        editor.pasteFromClipboard();
        QCOMPARE(editor.lineText(0), expected);
        QVERIFY(editor.getOptions().testFlag(QSynedit::EditorOption::AutoIndent));
        editor.undo();
        QCOMPARE(editor.lineText(0), before);
        editor.redo();
        QCOMPARE(editor.lineText(0), expected);
    }
    void pointerSizeReachesCompileAndLink() {
        UiRegressionContext ui;
        CompilerArgumentProbe compiler;
        for (const QString& size : {QString("32"), QString("64"), QString("x32")}) {
            compiler.set->setCompileOptions({{CC_CMD_OPT_POINTER_SIZE,size},
                                            {CC_CMD_OPT_PROFILE_INFO,COMPILER_OPTION_ON},
                                            {LINK_CMD_OPT_STRIP_EXE,COMPILER_OPTION_ON}});
            const QString option = "-m" + size;
            const auto c = compiler.getCCompileArguments(false);
            const auto cpp = compiler.getCppCompileArguments(false);
            const auto link = compiler.getLibraryArguments(FileType::Project);
            QVERIFY2(c.contains(option), qPrintable(option));
            QVERIFY2(cpp.contains(option), qPrintable(option));
            QVERIFY2(link.contains(option), qPrintable(option));
            // Keep other options' existing compile/link classification unchanged.
            QVERIFY(!c.contains("-pg") && !cpp.contains("-pg"));
            QVERIFY(!c.contains("-s") && !cpp.contains("-s"));
            QVERIFY(link.contains("-pg") && link.contains("-s"));
        }
    }
    void referenceLinksRespectLanguage() {
        UiRegressionContext ui;
        ReferenceUrlCapture capture;
        QDesktopServices::setUrlHandler("https", &capture, "capture");
        const auto reset = qScopeGuard([] { QDesktopServices::unsetUrlHandler("https"); });
        for (const QString& language : {QString("zh_CN"), QString("en_US")}) {
            ui.settings.environment().setLanguage(language);
            const QString host = language == "zh_CN" ? "cppreference.cn" : "en.cppreference.com";
            QVERIFY(QMetaObject::invokeMethod(ui.window.get(), "on_actionC_C_Reference_triggered", Qt::DirectConnection));
            QCOMPARE(capture.lastUrl, QUrl("https://"+host+"/w/cpp"));
            QVERIFY(QMetaObject::invokeMethod(ui.window.get(), "on_actionC_Reference_triggered", Qt::DirectConnection));
            QCOMPARE(capture.lastUrl, QUrl("https://"+host+"/w/c"));
        }
    }
#ifdef Q_OS_WIN
    void gdbServerDefaultAndLegacyMigration_data() {
        QTest::addColumn<int>("legacyValue");
        QTest::newRow("missing") << -1;
        QTest::newRow("disabled") << 0;
        QTest::newRow("enabled") << 1;
    }
    void gdbServerDefaultAndLegacyMigration() {
        QFETCH(int, legacyValue);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString filename = directory.filePath("settings.ini");
        {
            QSettings old(filename, QSettings::IniFormat);
            if (legacyValue >= 0)
                old.setValue("Debugger/use_gdb_server", legacyValue != 0);
            old.setValue("Debugger/gdb_server_port", 45678);
            old.setValue("Debugger/array_elements", 73);
            old.setValue("CompilerSet_0/debugger", "custom/gdb.exe");
            old.setValue("Environment/language", "zh_CN");
            old.sync();
            QCOMPARE(old.status(), QSettings::NoError);
        }
        {
            SettingsPersistor persistor(filename);
            DebuggerSettings settings(&persistor);
            settings.load();
            QVERIFY(settings.useGDBServer());
            QCOMPARE(settings.GDBServerPort(), 45678);
            QCOMPARE(settings.arrayElements(), 73);
        }
        {
            // Migration must already be persisted, even without saving options.
            QSettings migrated(filename, QSettings::IniFormat);
            QVERIFY(migrated.value("Debugger/use_gdb_server").toBool());
            QCOMPARE(migrated.value("Debugger/gdb_server_default_version").toInt(), 1);
            QCOMPARE(migrated.value("CompilerSet_0/debugger").toString(), QString("custom/gdb.exe"));
            QCOMPARE(migrated.value("Environment/language").toString(), QString("zh_CN"));
        }
        {
            SettingsPersistor persistor(filename);
            DebuggerSettings settings(&persistor);
            settings.load();
            settings.setUseGDBServer(false);
            settings.save();
            QCOMPARE(persistor.sync(), QSettings::NoError);
        }
        {
            SettingsPersistor persistor(filename);
            DebuggerSettings settings(&persistor);
            settings.load();
            QVERIFY(!settings.useGDBServer());
            QCOMPARE(settings.GDBServerPort(), 45678);
        }
    }
    void gdbServerDefaultWithoutProfile() {
        DebuggerSettings settings(nullptr);
        settings.load();
        QVERIFY(settings.useGDBServer());
    }
#endif
    void breakpointAddedWhileDebugging() {
        Debugger debugger;
        debugger.setIsForProject(true);
        auto *client = attach(debugger);
        client->setRunning(true);
        debugger.addBreakpoint(42, "control.c", true);
        QCOMPARE(client->mCmdQueue.size(), 1);
        QCOMPARE(client->mCmdQueue.head()->command, QString("-break-insert"));
        // Since 3.5, editor/model lines are zero-based; GDB lines are one-based.
        QVERIFY(client->mCmdQueue.head()->params.contains("--line 43"));
        client->mCmdQueue.clear();
        debugger.breakpointModel()->breakpoint(0,true)->number = 11;
        debugger.setBreakPointCondition(0, "counter > 3", true);
        QCOMPARE(client->mCmdQueue.head()->command, QString("-break-condition"));
        QCOMPARE(client->mCmdQueue.head()->params, QString("11 counter > 3"));
    }
    void runToCursorUsesOneBasedLine() {
        Debugger debugger;
        auto *client = attach(debugger);
        debugger.runTo("C:/control project/control.c", 42);
        QCOMPARE(client->mCmdQueue.head()->command, QString("-exec-until"));
        QCOMPARE(client->mCmdQueue.head()->params, QString("\"C:/control project/control.c:43\""));
    }
    void expressionsPreserveQuotedStrings() {
        Debugger debugger;
        auto *client = attach(debugger);
        const QString expression = "sizeof(\"a b\") == 4";
        const QString quoted = "\"sizeof(\\\"a b\\\") == 4\"";
        client->evalExpression(expression);
        QCOMPARE(client->mCmdQueue.dequeue()->params, quoted);
        client->addWatch(expression);
        QCOMPARE(client->mCmdQueue.dequeue()->params, quoted);
        client->readMemory("&watch_value + 1", 1, 3);
        QCOMPARE(client->mCmdQueue.dequeue()->params, QString("\"&watch_value + 1\" 3"));
        auto breakpoint = std::make_shared<Breakpoint>();
        breakpoint->number = 7;
        breakpoint->line = 8;
        breakpoint->filename = "control.c";
        breakpoint->condition = expression;
        client->addBreakpoint(breakpoint);
        QVERIFY(client->mCmdQueue.dequeue()->params.contains("-c "+quoted));
        client->setBreakpointCondition(breakpoint);
        // GDB consumes -break-condition's remainder as a raw C expression.
        QCOMPARE(client->mCmdQueue.dequeue()->params, "7 "+expression);
    }
    void memoryBlocksKeepTheirReportedAddresses_data() {
        QTest::addColumn<QByteArray>("block");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("short-row")
                << QByteArray("memory=[{begin=\"0x1000\",offset=\"0x0\",end=\"0x1003\",contents=\"010203\"}]")
                << QStringList({"0x1000 0x01 0x02 0x03"});
        QTest::newRow("nonzero-block-offset")
                << QByteArray("memory=[{begin=\"0x1010\",offset=\"0x10\",end=\"0x1013\",contents=\"010203\"}]")
                << QStringList({"0x1010 0x01 0x02 0x03"});
        QTest::newRow("full-and-short-rows")
                << QByteArray("memory=[{begin=\"0x1000\",offset=\"0x0\",end=\"0x1012\",contents=\"000102030405060708090a0b0c0d0e0f1011\"}]")
                << QStringList({"0x1000 0x00 0x01 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0a 0x0b 0x0c 0x0d 0x0e 0x0f", "0x1010 0x10 0x11"});
    }
    void memoryBlocksKeepTheirReportedAddresses() {
        QFETCH(QByteArray, block);
        QFETCH(QStringList, expected);
        UiRegressionContext ui;
        ui.settings.debugger().setMemoryViewColumns(16);
        Debugger debugger;
        auto *client = attach(debugger);
        client->mCurrentCmd = std::make_shared<GDBMICommand>();
        client->mCurrentCmd->command = "-data-read-memory-bytes";
        QSignalSpy memory(client, &DebuggerClient::memoryUpdated);
        client->processResult(block);
        QCOMPARE(memory.count(), 1);
        QCOMPARE(memory[0][0].toStringList(), expected);
    }
    void memoryWritesUseActualRowAndRejectOverflow() {
        MemoryModel model(16);
        model.updateMemory({"0x1000 0x01 0x02", "0x2000 0x03 0x04"});
        QSignalSpy writes(&model, &MemoryModel::setMemoryData);
        const auto index = model.index(1,1);
        QVERIFY(!model.setData(index, "100", Qt::EditRole));
        QVERIFY(writes.isEmpty());
        QVERIFY(model.setData(index, "ff", Qt::EditRole));
        QCOMPARE(writes.count(), 1);
        QCOMPARE(writes[0][0].toULongLong(), qulonglong(0x2001));
        QCOMPARE(writes[0][1].value<unsigned char>(), static_cast<unsigned char>(255));
    }
    void memoryWritesUseSparseRowAddress() {
        MemoryModel model(16);
        model.updateMemory({"0x1000 0x01 0x02", "0x2000 0x03 0x04"});
        QSignalSpy writes(&model, &MemoryModel::setMemoryData);
        QVERIFY(model.setData(model.index(1,1), "7f", Qt::EditRole));
        QCOMPARE(writes[0][0].toULongLong(), qulonglong(0x2001));
    }
    void runningBreakpointChangeWaitsForStop() {
        Debugger debugger;
        debugger.setIsForProject(true);
        debugger.setUseDebugServer(true);
        auto *client = attach(debugger);
        client->setRunning(true);
        debugger.addBreakpoint(42, "control.c", true);
        auto interrupt = client->takeNextCommand();
        QCOMPARE(interrupt->command, QString("-exec-interrupt"));
        QCOMPARE(client->mCmdQueue.size(), 1);
        QVERIFY(!client->takeNextCommand());
        client->setRunning(false);
        auto operation = client->takeNextCommand();
        QCOMPARE(operation->command, QString("-break-insert"));
        QVERIFY(client->mCmdQueue.isEmpty());
        QVERIFY(!client->mBreakpointInterruptPending);
    }
    void runningWatchpointWaitsForStop() {
        Debugger debugger;
        debugger.setUseDebugServer(true);
        auto *client = attach(debugger);
        client->setRunning(true);
        client->addWatchpoint("watch_value");
        QCOMPARE(client->takeNextCommand()->command, QString("-exec-interrupt"));
        QVERIFY(!client->takeNextCommand());
        client->setRunning(false);
        QCOMPARE(client->takeNextCommand()->command, QString("-break-watch"));
    }
    void internalPauseResumesOnlyAfterApplyingChanges() {
        Debugger debugger;
        debugger.setIsForProject(true);
        debugger.setUseDebugServer(true);
        auto *client = attach(debugger);
        QSignalSpy stopped(client, &DebuggerClient::inferiorStopped);
        client->setRunning(true);
        debugger.addBreakpoint(42, "control.c", true);
        QCOMPARE(client->takeNextCommand()->command, QString("-exec-interrupt"));
        client->processExecAsyncRecord("*stopped,reason=\"signal-received\",signal-name=\"SIGINT\",signal-meaning=\"Interrupt\"");
        QCOMPARE(stopped.count(), 0);
        QVERIFY(client->mResumeAfterBreakpointEdit);
        QCOMPARE(client->takeNextCommand()->command, QString("-break-insert"));
        QCOMPARE(client->takeNextCommand()->command, QString("-exec-continue"));
        QVERIFY(!client->takeNextCommand());
    }
    void realStopAndUserPauseCancelAutoResume() {
        Debugger debugger;
        debugger.setIsForProject(true);
        debugger.setUseDebugServer(true);
        auto *client = attach(debugger);
        QSignalSpy stopped(client, &DebuggerClient::inferiorStopped);
        client->setRunning(true);
        debugger.addBreakpoint(42, "control.c", true);
        client->takeNextCommand();
        client->processExecAsyncRecord("*stopped,reason=\"breakpoint-hit\",frame={addr=\"0x1\",func=\"tick\",fullname=\"control.c\",line=\"42\"}");
        QVERIFY(!client->mResumeAfterBreakpointEdit);
        QCOMPARE(stopped.count(), 1);
        client->mCmdQueue.clear();
        QVERIFY(!client->takeNextCommand());
        client->setRunning(true);
        debugger.addBreakpoint(43, "control.c", true);
        client->takeNextCommand();
        client->postCommand("-exec-interrupt", "");
        client->processExecAsyncRecord("*stopped,reason=\"signal-received\",signal-name=\"SIGINT\",signal-meaning=\"Interrupt\"");
        QVERIFY(!client->mResumeAfterBreakpointEdit);
        QCOMPARE(stopped.count(), 2);
    }
    void breakpointErrorAndOriginallyPausedStayPaused() {
        Debugger debugger;
        debugger.setIsForProject(true);
        auto *client = attach(debugger);
        debugger.addBreakpoint(42, "control.c", true);
        QCOMPARE(client->takeNextCommand()->command, QString("-break-insert"));
        QVERIFY(!client->takeNextCommand());
        QSignalSpy stopped(client, &DebuggerClient::inferiorStopped);
        client->mCurrentLine = 42;
        client->mResumeAfterBreakpointEdit = true;
        client->processError("^error,msg=\"No line 42\"");
        QVERIFY(!client->mResumeAfterBreakpointEdit);
        QCOMPARE(stopped.count(), 1);
    }
    void clearFileAndAllSynchronizeBackend() {
        Debugger debugger;
        debugger.setIsForProject(true);
        auto *client = attach(debugger);
        debugger.addBreakpoint(12, "a.c", true);
        debugger.addBreakpoint(20, "b.c", true);
        debugger.breakpointModel()->breakpoint(0,true)->number = 7;
        debugger.breakpointModel()->breakpoint(1,true)->number = 8;
        client->mCmdQueue.clear();
        debugger.deleteBreakpoints("a.c", true);
        QCOMPARE(client->mCmdQueue.size(), 1);
        QCOMPARE(client->mCmdQueue.head()->command, QString("-break-delete"));
        QCOMPARE(client->mCmdQueue.head()->params, QString("7"));
        QCOMPARE(debugger.breakpointModel()->rowCount(QModelIndex()), 1);
        client->mCmdQueue.clear();
        debugger.deleteBreakpoints(true);
        QCOMPARE(client->mCmdQueue.size(), 1);
        QCOMPARE(client->mCmdQueue.head()->params, QString("8"));
        QCOMPARE(debugger.breakpointModel()->rowCount(QModelIndex()), 0);
    }
    void watchEditRequiresPausedValidLeaf() {
        Debugger debugger;
        debugger.addWatchVar("counter");
        auto model = debugger.watchModel();
        auto valueIndex = model->index(0, 2, QModelIndex());
        QVERIFY(!(model->flags(valueIndex) & Qt::ItemIsEditable));
        model->updateVarInfo("counter", "var1", 0, "10", "int", false);
        QVERIFY(!(model->flags(valueIndex) & Qt::ItemIsEditable));
        model->setValueEditingEnabled(true);
        QVERIFY(model->flags(valueIndex) & Qt::ItemIsEditable);
        QCOMPARE(model->data(valueIndex, Qt::EditRole).toString(), QString("10"));
        QSignalSpy spy(model.get(), &WatchModel::setWatchVarValue);
        QVERIFY(model->setData(valueIndex, "42", Qt::EditRole));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QString("var1"));
        QCOMPARE(spy.at(0).at(1).toString(), QString("42"));
        QCOMPARE(model->data(valueIndex, Qt::DisplayRole).toString(), QString("10"));
        model->updateVarValue("var1", "42", "true", false, "", -1, false);
        QCOMPARE(model->data(valueIndex, Qt::DisplayRole).toString(), QString("42"));
        QVERIFY(!model->setData(valueIndex, " ", Qt::EditRole));
        QVERIFY(!model->setData(model->index(0,0,QModelIndex()), "1", Qt::EditRole));
        model->updateVarValue("var1", "", "false", false, "", -1, false);
        QVERIFY(!(model->flags(valueIndex) & Qt::ItemIsEditable));
        QVERIFY(!model->setData(valueIndex, "43", Qt::EditRole));
        model->resetAllVarInfos();
        QVERIFY(!(model->flags(model->index(0,2,QModelIndex())) & Qt::ItemIsEditable));
    }
    void aggregateAndChildEditing() {
        Debugger debugger;
        debugger.addWatchVar("pair");
        auto model = debugger.watchModel();
        model->updateVarInfo("pair", "var1", 1, "{...}", "Pair", false);
        model->addVarChild("var1", "var1.first", "first", 0, "3", "int", false);
        model->setValueEditingEnabled(true);
        auto parent = model->index(0,0,QModelIndex());
        QVERIFY(!(model->flags(model->index(0,2,QModelIndex())) & Qt::ItemIsEditable));
        auto child = model->index(0,2,parent);
        QVERIFY(model->flags(child) & Qt::ItemIsEditable);
        model->setValueEditingEnabled(false);
        QVERIFY(!(model->flags(child) & Qt::ItemIsEditable));
    }
    void valueAssignmentIsOneEscapedMiCommand() {
        Debugger debugger;
        TestClient client(&debugger);
        client.writeWatchVar("var1", "counter + 1");
        QCOMPARE(client.mCmdQueue.size(), 1);
        QCOMPARE(client.mCmdQueue.dequeue()->params, QString("var1 \"counter + 1\""));
        client.writeWatchVar("var1", "\"a\\b\"\n");
        auto command = client.mCmdQueue.dequeue();
        QCOMPARE(command->command, QString("-var-assign"));
        QVERIFY(!command->params.contains('\n'));
        QVERIFY(command->params.contains("\\n"));
        QVERIFY(command->params.contains("\\\""));
        QVERIFY(command->params.contains("\\\\"));
    }
    void staleValueEditIsRejectedWhileRunning() {
        Debugger debugger;
        auto *client = attach(debugger);
        client->setRunning(true);
        debugger.setWatchVarValue("var1", "42");
        QVERIFY(client->mCmdQueue.isEmpty());
    }
};

QTEST_MAIN(DebugWatchTest)
#include "debugwatchtest.moc"
