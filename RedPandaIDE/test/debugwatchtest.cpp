#include <QtTest>
#include "debugger/debugger.h"
#include "debugger/gdbmidebugger.h"
#include "settings/debuggersettings.h"

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
