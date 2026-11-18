#include <QtTest>
#include <QElapsedTimer>

#include "core/lua_sandbox.h"

// 漏洞自定义 UI 的 Lua 沙箱：
//   - ui.* / device.* / log() 的约定；
//   - io/os/package/debug 与文件加载入口不可达；
//   - 死循环由指令 hook 在 2s 内掐断。
class TestLuaSandbox : public QObject
{
    Q_OBJECT

private slots:
    void runSetsHtmlAndActions();
    void sandboxHasNoFileOrOsAccess();
    void timeoutStopsInfiniteLoop();
    void invokeActionUpdatesHtml();
    void syntaxErrorReported();
    void deviceSnapshotVisible();
    void htmlSizeCapped();
};

void TestLuaSandbox::runSetsHtmlAndActions()
{
    LuaSandbox sandbox;
    QString error;
    QVERIFY2(sandbox.run(R"LUA(
        ui.set_html("<b>hello</b>")
        ui.action("check", "检测")
        ui.action("run", "执行")
    )LUA", &error), qPrintable(error));

    QCOMPARE(sandbox.html(), QStringLiteral("<b>hello</b>"));
    QCOMPARE(sandbox.actions().size(), 2);
    QCOMPARE(sandbox.actions().at(0).id, QStringLiteral("check"));
    QCOMPARE(sandbox.actions().at(1).label, QStringLiteral("执行"));
}

void TestLuaSandbox::sandboxHasNoFileOrOsAccess()
{
    LuaSandbox sandbox;
    QString error;

    QVERIFY(!sandbox.run("os.execute('true')", &error));
    QVERIFY2(error.contains(QStringLiteral("os")), qPrintable(error));

    QVERIFY(!sandbox.run("return io.open('/etc/passwd')", &error));
    QVERIFY2(error.contains(QStringLiteral("io")), qPrintable(error));

    QVERIFY(!sandbox.run("return dofile('/etc/passwd')", &error));
    QVERIFY2(error.contains(QStringLiteral("dofile")), qPrintable(error));
}

void TestLuaSandbox::timeoutStopsInfiniteLoop()
{
    LuaSandbox sandbox;
    QString error;

    QElapsedTimer timer;
    timer.start();
    QVERIFY(!sandbox.run("while true do end", &error));
    QVERIFY2(error.contains(QStringLiteral("超时")), qPrintable(error));
    // 2s 上限 + 余量，绝不能真的卡死。
    QVERIFY(timer.elapsed() < 5000);
}

void TestLuaSandbox::invokeActionUpdatesHtml()
{
    LuaSandbox sandbox;
    QString error;
    QVERIFY2(sandbox.run(R"LUA(
        ui.set_html("idle")
        ui.action("go", "Go")
        function on_action(id)
            if id == "go" then ui.set_html("clicked") end
        end
    )LUA", &error), qPrintable(error));

    QCOMPARE(sandbox.html(), QStringLiteral("idle"));
    QVERIFY2(sandbox.invokeAction("go", &error), qPrintable(error));
    QCOMPARE(sandbox.html(), QStringLiteral("clicked"));
}

void TestLuaSandbox::syntaxErrorReported()
{
    LuaSandbox sandbox;
    QString error;
    QVERIFY(!sandbox.run("function (", &error));
    QVERIFY(!error.isEmpty());
}

void TestLuaSandbox::deviceSnapshotVisible()
{
    LuaSandbox sandbox;
    LuaSandbox::DeviceSnapshot device;
    device.model = QStringLiteral("Pixel Test");
    device.serial = QStringLiteral("ABC123");
    device.androidVersion = QStringLiteral("14");
    sandbox.setDevice(device);

    QString error;
    QVERIFY2(sandbox.run(
                 "ui.set_html(device.model .. '/' .. device.serial .. '/Android ' "
                 ".. device.android_version)",
                 &error),
             qPrintable(error));
    QCOMPARE(sandbox.html(), QStringLiteral("Pixel Test/ABC123/Android 14"));
}

void TestLuaSandbox::htmlSizeCapped()
{
    LuaSandbox sandbox;
    QString error;
    QVERIFY(!sandbox.run("ui.set_html(string.rep('x', 300000))", &error));
    QVERIFY(!error.isEmpty());
}

QTEST_APPLESS_MAIN(TestLuaSandbox)
#include "test_lua_sandbox.moc"
