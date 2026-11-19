#ifndef LUA_SANDBOX_H
#define LUA_SANDBOX_H

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QString>

#include <functional>

struct lua_State;

// 漏洞自定义 UI 的 Lua 沙箱（vendored Lua 5.4）。
//
// 安全边界：
//   - 只打开 base / string / table / math / utf8 / coroutine；
//   - 不开 io / os / package / debug，并移除 dofile / loadfile / load；
//   - 指令计数 hook：单次执行超过 kTimeoutMs 直接报错，防死循环卡死界面；
//   - 对外只暴露 ui.* / device.* / log()。
//
// 脚本约定：
//   ui.set_html("<h2>...</h2>")      —— 覆盖详情窗口的展示区
//   ui.action("id", "按钮文字")       —— 声明一个动作按钮
//   function on_action(id) ... end   —— 用户点按钮时调用
class LuaSandbox
{
public:
    struct Action {
        QString id;
        QString label;
        bool enabled = true;
    };

    // 表单控件：文本输入或复选框。宿主据此生成 Qt 控件，并把当前值
    // 通过 setValue() 回填，脚本用 ui.value(id) 读取。
    struct InputSpec {
        QString id;
        QString label;
        QString defaultValue;
        bool checkbox = false;
    };

    // 状态行：脚本声明一个命名状态位，宿主渲染成标签；
    // 运行中可以由脚本 ui.set_status 或宿主覆盖。
    struct StatusSpec {
        QString id;
        QString text;
    };

    struct DeviceSnapshot {
        QString serial;
        QString model;
        QString manufacturer;
        QString androidVersion;
        QString sdk;
        QString securityPatch;
    };

    // 宿主提供给 Lua 的 BROM 能力。界面与流程写在 Lua 里，C++ 只给底层操作。
    struct BromGate {
        bool canRun = false;
        QString chipSummary;
        QString reason;
    };

    struct BromApi {
        // 枚举当前 USB 上的 BROM/preloader 设备，返回给用户看的摘要。
        std::function<QString()> scan;
        // 芯片表查询（快，纯查询）。
        std::function<QString(const QString &hwCode, const QString &var1)> chipStatus;
        // 执行闸门：芯片码是否可用、载荷是否就绪。
        std::function<BromGate(const QString &hwCode, const QString &var1,
                               bool payloadReady)> gate;
        // 解析载荷（本地文件 / 显式允许的网络来源）。
        std::function<bool(const QString &hwCode, const QString &payloadPath,
                           bool allowNetwork, QString *message)> resolve;
        // 执行利用（调用方负责 Tier3 确认）。
        std::function<bool(const QString &hwCode, const QString &var1,
                           bool allowPatched, QString *message)> run;
    };

    LuaSandbox();
    ~LuaSandbox();
    LuaSandbox(const LuaSandbox &) = delete;
    LuaSandbox &operator=(const LuaSandbox &) = delete;

    void setDevice(const DeviceSnapshot &device) { m_device = device; }
    void setLogger(std::function<void(const QString &)> logger)
    {
        m_logger = std::move(logger);
    }
    void setBromApi(const BromApi &api) { m_bromApi = api; }
    const BromApi &bromApi() const { return m_bromApi; }

    // 执行整段脚本。语法/运行时/超时错误写入 *error。
    bool run(const QString &script, QString *error);

    // 调用脚本的 on_action(id)。脚本可再次 ui.set_html / ui.action / log。
    bool invokeAction(const QString &id, QString *error);

    QString html() const { return m_html; }
    QList<Action> actions() const { return m_actions; }
    QList<InputSpec> inputs() const { return m_inputs; }
    QList<StatusSpec> statuses() const { return m_statuses; }

    // 宿主 <-> 表单的值同步。
    QString value(const QString &id) const { return m_values.value(id); }
    void setValue(const QString &id, const QString &value) { m_values[id] = value; }

    // 供 Lua C API 回调 / 宿主渲染使用。
    void setHtml(const QString &html) { m_html = html; }
    void addAction(const QString &id, const QString &label);
    void addInput(const InputSpec &input);
    void setStatus(const QString &id, const QString &text);
    void setActionEnabled(const QString &id, bool enabled);
    void log(const QString &text);
    bool timedOut() const { return m_elapsed.elapsed() > kTimeoutMs; }

    static constexpr int kTimeoutMs = 2000;
    static constexpr int kMaxHtmlBytes = 256 * 1024;

private:
    void registerApi();
    void pushDeviceTable();
    void resetOutput();
    bool protectedCall(int nargs, int nresults, QString *error);
    void setError(QString *error, const char *fallback);

    lua_State *m_state = nullptr;
    QString m_html;
    QList<Action> m_actions;
    QList<InputSpec> m_inputs;
    QList<StatusSpec> m_statuses;
    QHash<QString, QString> m_values;
    DeviceSnapshot m_device;
    BromApi m_bromApi;
    std::function<void(const QString &)> m_logger;
    QElapsedTimer m_elapsed;
};

#endif // LUA_SANDBOX_H
