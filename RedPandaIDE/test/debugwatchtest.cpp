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
#include <qt_utils/charsetinfo.h>
#include <qsynedit/syntaxer/cpp.h>
#include <qsynedit/formatter/cppformatter.h>
#include <QClipboard>
#include <QDesktopServices>

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
