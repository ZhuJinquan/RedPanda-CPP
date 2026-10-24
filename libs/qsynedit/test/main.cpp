#include <QtTest>
#include <QGuiApplication>
#include <QDir>
#include "test_charpos.h"
#include "test_cppsyntaxer.h"
#include "test_document.h"
#include "test_qsynedit_cpp.h"
#include "test_qsynedit_emoji.h"
#include "test_qsynedit_cpp_search_replace.h"
#include "qsynedit/types.h"

int main(int argc, char *argv[]) {
    int status = 0;
    QTest::setMainSourcePath(__FILE__, QT_TESTCASE_BUILDDIR); // Optional: for source path resolution

    QApplication app(argc,argv);
    QStringList arguments = app.arguments();
    QString reportDirectory;
    const int reportIndex = arguments.indexOf("--report-dir");
    if (reportIndex >= 0 && reportIndex + 1 < arguments.size()) {
        reportDirectory = arguments[reportIndex + 1];
        QDir().mkpath(reportDirectory);
        arguments.removeAt(reportIndex + 1);
        arguments.removeAt(reportIndex);
    }
    auto run = [&](QObject &test) {
        if (reportDirectory.isEmpty())
            return QTest::qExec(&test, argc, argv);
        QString name = QString::fromLatin1(test.metaObject()->className());
        name.replace(":", "_");
        QStringList testArguments = arguments;
        testArguments << "-o" << (QDir(reportDirectory).filePath(name + ".txt") + ",txt");
        return QTest::qExec(&test, testArguments);
    };
    //CharPos Test
    {
        QSynedit::TestCharPos tc;
        status |= run(tc);
    }
    {
        QSynedit::TestDocumentHelpers tc;
        status |= run(tc);
    }

    //QDocument Test
    {
        QSynedit::TestDocument tc;
        status |= run(tc);
    }

    //Cpp Syntaxer Test
    {
        TestCppSyntaxer tc;
        status |= run(tc);
    }

    //SynEdit Test
    {
        QSynedit::TestQSyneditCpp tc;
        status |= run(tc);
    }

    {
        QSynedit::TestQSyneditEmoji tc;
        status |= run(tc);
    }
    {
        QSynedit::TestQSyneditCppSearchReplace tc;
        status |= run(tc);
    }

    return status;
}
